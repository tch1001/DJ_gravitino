// TrackLibrary — folder scan, background analysis, JSON cache, table model.
// Owned by claude-analysis. See docs/ARCHITECTURE.md ("library").
//
// The pinned header exposes no data members, so per-instance state lives in a
// file-local registry keyed by the model pointer (cleaned up on destroyed()).

#include "TrackLibrary.h"
#include "../analysis/AnalysisInternal.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrent>

#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace gvt {

namespace {

enum Col { ColTitle = 0, ColArtist, ColBpm, ColDuration, ColStatus, ColCount };

struct Row {
    QString      path;
    TrackDataPtr track;             // null until analyzed
    QString      status = QStringLiteral("analyzing…");
};

struct LibState {
    QThreadPool pool;
    std::vector<Row> rows;
    int total = 0;
    int analyzed = 0;               // GUI thread only
    int generation = 0;             // invalidates in-flight workers on rescan
    LibState() { pool.setMaxThreadCount(4); }
};

std::mutex g_regMutex;
std::unordered_map<const TrackLibrary*, std::shared_ptr<LibState>> g_registry;

std::shared_ptr<LibState> state(const TrackLibrary* m)
{
    std::lock_guard<std::mutex> lk(g_regMutex);
    auto& s = g_registry[m];
    if (!s) s = std::make_shared<LibState>();
    return s;
}

QString cacheDirPath()
{
    return QDir::homePath() + QStringLiteral("/.gravitino/cache");
}

QString cacheFileFor(const QString& trackPath)
{
    const QByteArray sha1 = QCryptographicHash::hash(trackPath.toUtf8(),
                                                     QCryptographicHash::Sha1).toHex();
    return cacheDirPath() + QLatin1Char('/') + QString::fromLatin1(sha1) + QStringLiteral(".json");
}

void writeCache(const TrackData& t, qint64 mtimeMs)
{
    QDir().mkpath(cacheDirPath());
    QJsonObject o;
    o[QStringLiteral("path")]         = t.filePath;
    o[QStringLiteral("mtime")]        = (double)mtimeMs;
    o[QStringLiteral("title")]        = t.title;
    o[QStringLiteral("artist")]       = t.artist;
    o[QStringLiteral("album")]        = t.album;
    o[QStringLiteral("durationSec")]  = t.durationSec;
    o[QStringLiteral("bpm")]          = t.bpm;
    o[QStringLiteral("firstBeatSec")] = t.firstBeatSec;
    o[QStringLiteral("fingerprint")]  = t.fingerprint;
    QJsonArray cues;
    for (double c : t.hotCues) cues.append(c);
    o[QStringLiteral("hotCues")] = cues;

    QSaveFile f(cacheFileFor(t.filePath));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

// Cache hit: decode PCM (needed for playback) but skip beat analysis.
TrackDataPtr loadFromCache(const QString& path, qint64 mtimeMs, QString* error)
{
    QFile f(cacheFileFor(path));
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return nullptr;
    const QJsonObject o = doc.object();
    if ((qint64)o.value(QStringLiteral("mtime")).toDouble(-1) != mtimeMs) return nullptr;

    auto t = std::make_shared<TrackData>();
    t->filePath = path;
    if (!detail::decodeMp3Stereo48k(path, t->pcm, error)) return nullptr;
    t->title        = o.value(QStringLiteral("title")).toString();
    t->artist       = o.value(QStringLiteral("artist")).toString();
    t->album        = o.value(QStringLiteral("album")).toString();
    t->bpm          = o.value(QStringLiteral("bpm")).toDouble();
    t->firstBeatSec = o.value(QStringLiteral("firstBeatSec")).toDouble();
    t->fingerprint  = o.value(QStringLiteral("fingerprint")).toString();
    t->durationSec  = (double)t->frameCount() / (double)kSampleRate;
    const QJsonArray cues = o.value(QStringLiteral("hotCues")).toArray();
    for (int i = 0; i < 8 && i < cues.size(); ++i) t->hotCues[i] = cues[i].toDouble(-1.0);
    if (t->title.isEmpty()) t->title = QFileInfo(path).completeBaseName();
    t->overviewPeaks = detail::computeOverviewPeaks(t->pcm);
    return t;
}

TrackDataPtr analyzeWithCache(const QString& path, QString* error)
{
    const qint64 mtimeMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    if (TrackDataPtr cached = loadFromCache(path, mtimeMs, error))
        return cached;
    TrackDataPtr t = loadAndAnalyzeTrack(path, error);
    if (t) writeCache(*t, mtimeMs);
    return t;
}

QString formatDuration(double sec)
{
    const int s = (int)std::lround(sec);
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

TrackLibrary::TrackLibrary(QObject* parent) : QAbstractTableModel(parent)
{
    state(this); // create per-instance state
    connect(this, &QObject::destroyed, [p = this] {
        std::lock_guard<std::mutex> lk(g_regMutex);
        g_registry.erase(p);
    });
}

void TrackLibrary::scanFolder(const QString& dirIn)
{
    auto st = state(this);
    const QString dirPath = dirIn.isEmpty() ? QDir::homePath() + QStringLiteral("/Music")
                                            : dirIn;
    // Non-recursive + one level of subdirectories.
    QStringList files;
    const QDir dir(dirPath);
    const QStringList mp3 = {QStringLiteral("*.mp3")};
    for (const QFileInfo& fi : dir.entryInfoList(mp3, QDir::Files | QDir::Readable, QDir::Name))
        files << fi.absoluteFilePath();
    for (const QFileInfo& sub : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        for (const QFileInfo& fi : QDir(sub.absoluteFilePath())
                                       .entryInfoList(mp3, QDir::Files | QDir::Readable, QDir::Name))
            files << fi.absoluteFilePath();

    st->generation++;
    const int gen = st->generation;

    beginResetModel();
    st->rows.clear();
    st->rows.reserve((size_t)files.size());
    for (const QString& f : files)
        st->rows.push_back(Row{f, nullptr, QStringLiteral("analyzing…")});
    st->total = (int)files.size();
    st->analyzed = 0;
    endResetModel();
    emit scanProgress(0, st->total);

    QPointer<TrackLibrary> self(this);
    for (int i = 0; i < (int)st->rows.size(); ++i) {
        const QString path = st->rows[(size_t)i].path;
        auto task = [st, self, gen, i, path] {
            QString err;
            TrackDataPtr t = analyzeWithCache(path, &err);
            TrackLibrary* obj = self.data();
            if (!obj) return;
            QMetaObject::invokeMethod(obj, [st, self, gen, i, t] {
                TrackLibrary* m = self.data();
                if (!m || gen != st->generation || i >= (int)st->rows.size()) return;
                st->rows[(size_t)i].track = t;
                st->rows[(size_t)i].status = t ? QStringLiteral("ready")
                                               : QStringLiteral("error");
                st->analyzed++;
                emit m->dataChanged(m->index(i, 0), m->index(i, ColCount - 1));
                emit m->trackReady(i);
                emit m->scanProgress(st->analyzed, st->total);
            }, Qt::QueuedConnection);
        };
        (void)QtConcurrent::run(&st->pool, std::move(task));
    }
}

int TrackLibrary::trackCount() const
{
    return (int)state(this)->rows.size();
}

TrackDataPtr TrackLibrary::trackAt(int row) const
{
    auto st = state(this);
    if (row < 0 || row >= (int)st->rows.size()) return nullptr;
    return st->rows[(size_t)row].track;
}

QString TrackLibrary::pathAt(int row) const
{
    auto st = state(this);
    if (row < 0 || row >= (int)st->rows.size()) return {};
    return st->rows[(size_t)row].path;
}

int TrackLibrary::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : (int)state(this)->rows.size();
}

int TrackLibrary::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColCount;
}

QVariant TrackLibrary::data(const QModelIndex& idx, int role) const
{
    auto st = state(this);
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= (int)st->rows.size())
        return {};
    const Row& r = st->rows[(size_t)idx.row()];

    if (role == Qt::TextAlignmentRole) {
        if (idx.column() == ColBpm || idx.column() == ColDuration)
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }
    if (role != Qt::DisplayRole) return {};

    switch (idx.column()) {
    case ColTitle:
        return r.track && !r.track->title.isEmpty()
                   ? r.track->title
                   : QFileInfo(r.path).completeBaseName();
    case ColArtist:
        return r.track ? r.track->artist : QString();
    case ColBpm:
        return (r.track && r.track->bpm > 0.0) ? QString::number(r.track->bpm, 'f', 1)
                                               : QString();
    case ColDuration:
        return r.track ? formatDuration(r.track->durationSec) : QString();
    case ColStatus:
        return r.status;
    }
    return {};
}

QVariant TrackLibrary::headerData(int section, Qt::Orientation o, int role) const
{
    if (role != Qt::DisplayRole || o != Qt::Horizontal) return {};
    switch (section) {
    case ColTitle:    return QStringLiteral("Title");
    case ColArtist:   return QStringLiteral("Artist");
    case ColBpm:      return QStringLiteral("BPM");
    case ColDuration: return QStringLiteral("Duration");
    case ColStatus:   return QStringLiteral("Status");
    }
    return {};
}

} // namespace gvt

// TrackLibrary — folder scan, background analysis, JSON cache, table model.
// Owned by claude-analysis. See docs/ARCHITECTURE.md ("library").
//
// The pinned header exposes no data members, so per-instance state lives in a
// file-local registry keyed by the model pointer (cleaned up on destroyed()).

#include "TrackLibrary.h"
#include "SongCatalog.h"
#include "../analysis/AnalysisInternal.h"
#include "../analysis/BeatGridEditor.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
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

enum Col { ColTitle = 0, ColArtist, ColBpm, ColKey, ColDuration, ColStatus, ColCount };

struct Row {
    QString      path;
    TrackDataPtr track;             // null until analyzed
    QString      status = QStringLiteral("analyzing…");
};

struct LibState {
    QThreadPool pool;
    SongCatalog catalog;
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
    const QByteArray overridePath = qgetenv("GRAVITINO_CACHE_DIR");
    if (!overridePath.isEmpty())
        return QFile::decodeName(overridePath);
    return QDir::homePath() + QStringLiteral("/.gravitino/cache");
}

QString cacheFileFor(const QString& trackPath)
{
    const QByteArray sha1 = QCryptographicHash::hash(trackPath.toUtf8(),
                                                     QCryptographicHash::Sha1).toHex();
    return cacheDirPath() + QLatin1Char('/') + QString::fromLatin1(sha1) + QStringLiteral(".json");
}

bool writeCache(const TrackData& t, qint64 mtimeMs, QString* error = nullptr,
                const TrackData* gridOverride = nullptr,
                const TrackData* performanceOverride = nullptr)
{
    if (error)
        error->clear();
    const QString dirPath = cacheDirPath();
    if (!QDir().mkpath(dirPath)) {
        if (error)
            *error = QStringLiteral("Could not create analysis cache: %1")
                         .arg(dirPath);
        return false;
    }
    QJsonObject o;
    o[QStringLiteral("path")]         = t.filePath;
    o[QStringLiteral("mtime")]        = (double)mtimeMs;
    o[QStringLiteral("title")]        = t.title;
    o[QStringLiteral("artist")]       = t.artist;
    o[QStringLiteral("album")]        = t.album;
    o[QStringLiteral("isrc")] = t.isrc;
    o[QStringLiteral("musicBrainzRecording")] = t.musicBrainzRecording;
    o[QStringLiteral("durationSec")]  = t.durationSec;
    o[QStringLiteral("bpm")] = gridOverride ? gridOverride->bpm : t.bpm;
    o[QStringLiteral("firstBeatSec")] = gridOverride
        ? gridOverride->firstBeatSec : t.firstBeatSec;
    o[QStringLiteral("fingerprint")]  = t.fingerprint;
    o[QStringLiteral("structureFingerprint")] = t.structureFingerprint;
    o[QStringLiteral("assetSha256")] = t.assetSha256;
    o[QStringLiteral("audibleDurationSec")] = t.audibleDurationSec;
    o[QStringLiteral("songId")] = t.songId;
    o[QStringLiteral("camelotKey")]   = t.camelotKey;
    o[QStringLiteral("keyName")]      = t.keyName;
    const TrackData& performance = performanceOverride
        ? *performanceOverride : t;
    QJsonArray cues;
    for (double c : performance.hotCues) cues.append(c);
    o[QStringLiteral("hotCues")] = cues;
    QJsonArray savedLoops;
    for (const SavedLoopSlot& slot : performance.savedLoops) {
        QJsonObject saved;
        saved[QStringLiteral("startSec")] = slot.startSec;
        saved[QStringLiteral("endSec")] = slot.endSec;
        saved[QStringLiteral("label")] = slot.label;
        savedLoops.append(saved);
    }
    o[QStringLiteral("savedLoops")] = savedLoops;

    QSaveFile f(cacheFileFor(t.filePath));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Could not open analysis cache: %1")
                         .arg(f.errorString());
        return false;
    }
    const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Indented);
    if (f.write(json) != json.size()) {
        if (error)
            *error = QStringLiteral("Could not write analysis cache: %1")
                         .arg(f.errorString());
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("Could not save analysis cache: %1")
                         .arg(f.errorString());
        return false;
    }
    return true;
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
    // Entries written before portable structural identity existed re-analyze
    // once so alternate encodes can be grouped safely.
    if (!o.contains(QStringLiteral("camelotKey")) ||
        !o.contains(QStringLiteral("isrc")) ||
        !o.contains(QStringLiteral("musicBrainzRecording")) ||
        !o.value(QStringLiteral("structureFingerprint")).toString()
             .startsWith(QStringLiteral("gvsf2:"))) return nullptr;

    auto t = std::make_shared<TrackData>();
    t->filePath = path;
    if (!detail::decodeAudioStereo48k(path, t->pcm, error)) return nullptr;
    t->title        = o.value(QStringLiteral("title")).toString();
    t->artist       = o.value(QStringLiteral("artist")).toString();
    t->album        = o.value(QStringLiteral("album")).toString();
    t->isrc = o.value(QStringLiteral("isrc")).toString();
    t->musicBrainzRecording =
        o.value(QStringLiteral("musicBrainzRecording")).toString();
    t->bpm          = o.value(QStringLiteral("bpm")).toDouble();
    t->firstBeatSec = o.value(QStringLiteral("firstBeatSec")).toDouble();
    t->fingerprint  = o.value(QStringLiteral("fingerprint")).toString();
    t->structureFingerprint =
        o.value(QStringLiteral("structureFingerprint")).toString();
    t->assetSha256 = o.value(QStringLiteral("assetSha256")).toString();
    t->audibleDurationSec =
        o.value(QStringLiteral("audibleDurationSec")).toDouble();
    t->songId = o.value(QStringLiteral("songId")).toString();
    t->camelotKey   = o.value(QStringLiteral("camelotKey")).toString();
    t->keyName      = o.value(QStringLiteral("keyName")).toString();
    t->durationSec  = (double)t->frameCount() / (double)kSampleRate;
    const QJsonArray cues = o.value(QStringLiteral("hotCues")).toArray();
    for (int i = 0; i < 8 && i < cues.size(); ++i) t->hotCues[i] = cues[i].toDouble(-1.0);
    const QJsonArray savedLoops =
        o.value(QStringLiteral("savedLoops")).toArray();
    for (int i = 0; i < 8 && i < savedLoops.size(); ++i) {
        const QJsonObject saved = savedLoops.at(i).toObject();
        SavedLoopSlot slot;
        slot.startSec = saved.value(QStringLiteral("startSec")).toDouble(-1.0);
        slot.endSec = saved.value(QStringLiteral("endSec")).toDouble(-1.0);
        slot.label = saved.value(QStringLiteral("label")).toString();
        if (slot.isSet())
            t->savedLoops[i] = slot;
    }
    if (t->title.isEmpty()) t->title = QFileInfo(path).completeBaseName();
    t->overviewPeaks = detail::computeOverviewPeaks(t->pcm);
    detail::computeBandOverviews(t->pcm, t->overviewLow, t->overviewMid, t->overviewHigh);
    return t;
}

TrackDataPtr analyzeWithCache(const QString& path, QString* error)
{
    const qint64 mtimeMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    if (TrackDataPtr cached = loadFromCache(path, mtimeMs, error))
        return cached;
    TrackDataPtr t = loadAndAnalyzeTrack(path, error);
    if (t) (void)writeCache(*t, mtimeMs);
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
    // Recursive scan, skipping hidden directories.
    QStringList files;
    QDirIterator it(dirPath,
                    {QStringLiteral("*.mp3"), QStringLiteral("*.flac"),
                     QStringLiteral("*.wav"), QStringLiteral("*.aif"),
                     QStringLiteral("*.aiff")},
                    QDir::Files | QDir::Readable,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        if (!p.contains(QStringLiteral("/."))) files << p;
    }
    files.sort();

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
                if (t) {
                    t->songId = st->catalog.registerAsset(*t);
                    t->canonicalBeatOffset =
                        st->catalog.canonicalBeatOffsetForAsset(t->filePath);
                }
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

QStringList TrackLibrary::compatibleAssetPaths(int row) const
{
    const TrackDataPtr track = trackAt(row);
    return track ? state(this)->catalog.assetsForSong(track->songId)
                 : QStringList{};
}

std::vector<CatalogTransitionLink>
TrackLibrary::transitionsForTrack(int row) const
{
    const TrackDataPtr track = trackAt(row);
    return track ? state(this)->catalog.transitionsForSong(track->songId)
                 : std::vector<CatalogTransitionLink>{};
}

void TrackLibrary::rebuildTransitionGraph(const TransitionStore& transitions)
{
    state(this)->catalog.rebuildTransitionGraph(transitions.all());
}

SongCatalog* TrackLibrary::songCatalog()
{
    return &state(this)->catalog;
}

bool TrackLibrary::persistBeatGrid(const TrackData& corrected, QString* error)
{
    if (error)
        error->clear();
    if (corrected.filePath.isEmpty()) {
        if (error) *error = QStringLiteral("Track has no file path");
        return false;
    }
    if (!BeatGridEditor::isValidBpm(corrected.bpm)) {
        if (error) {
            *error = QStringLiteral("BPM must be between %1 and %2")
                         .arg(BeatGridEditor::kMinBpm, 0, 'f', 1)
                         .arg(BeatGridEditor::kMaxBpm, 0, 'f', 1);
        }
        return false;
    }
    if (!std::isfinite(corrected.firstBeatSec)) {
        if (error) *error = QStringLiteral("Beat-grid anchor is not finite");
        return false;
    }

    auto st = state(this);
    int row = -1;
    for (int index = 0; index < static_cast<int>(st->rows.size()); ++index) {
        if (st->rows[static_cast<std::size_t>(index)].path ==
            corrected.filePath) {
            row = index;
            break;
        }
    }
    if (row < 0) {
        if (error) *error = QStringLiteral("Track is not in this library");
        return false;
    }

    Row& libraryRow = st->rows[static_cast<std::size_t>(row)];
    if (!libraryRow.track) {
        if (error) *error = QStringLiteral("Track is still being analyzed");
        return false;
    }

    // The loaded deck may hold an older TrackData instance after a rescan.
    // Serialize the library's current metadata/hot cues and override only the
    // two corrected grid fields, so a regrid cannot resurrect stale analysis.
    // QSaveFile commits the merged object atomically.
    const qint64 mtimeMs =
        QFileInfo(corrected.filePath).lastModified().toMSecsSinceEpoch();
    if (!writeCache(*libraryRow.track, mtimeMs, error, &corrected))
        return false;

    libraryRow.track->bpm = corrected.bpm;
    libraryRow.track->firstBeatSec = corrected.firstBeatSec;
    emit dataChanged(index(row, ColBpm), index(row, ColBpm),
                     {Qt::DisplayRole});
    return true;
}

bool TrackLibrary::persistPerformanceMetadata(
    const TrackData& updated, QString* error)
{
    if (error)
        error->clear();
    if (updated.filePath.isEmpty()) {
        if (error) *error = QStringLiteral("Track has no file path");
        return false;
    }

    auto st = state(this);
    int row = -1;
    for (int index = 0; index < static_cast<int>(st->rows.size()); ++index) {
        if (st->rows[static_cast<std::size_t>(index)].path ==
            updated.filePath) {
            row = index;
            break;
        }
    }
    if (row < 0) {
        if (error) *error = QStringLiteral("Track is not in this library");
        return false;
    }

    Row& libraryRow = st->rows[static_cast<std::size_t>(row)];
    if (!libraryRow.track) {
        if (error) *error = QStringLiteral("Track is still being analyzed");
        return false;
    }

    const qint64 mtimeMs =
        QFileInfo(updated.filePath).lastModified().toMSecsSinceEpoch();
    if (!writeCache(*libraryRow.track, mtimeMs, error, nullptr, &updated))
        return false;

    for (int pad = 0; pad < 8; ++pad) {
        libraryRow.track->hotCues[pad] = updated.hotCues[pad];
        libraryRow.track->savedLoops[pad] = updated.savedLoops[pad].isSet()
            ? updated.savedLoops[pad] : SavedLoopSlot {};
    }
    return true;
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
        if (idx.column() == ColBpm || idx.column() == ColKey || idx.column() == ColDuration)
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
    case ColKey:
        return r.track ? r.track->camelotKey : QString();
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
    case ColKey:      return QStringLiteral("Key");
    case ColDuration: return QStringLiteral("Duration");
    case ColStatus:   return QStringLiteral("Status");
    }
    return {};
}

} // namespace gvt

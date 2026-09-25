// TrackLibrary — folder scan, background analysis, JSON cache, table model.
// Owned by claude-analysis. See docs/ARCHITECTURE.md ("library").
//
// The pinned header exposes no data members, so per-instance state lives in a
// file-local registry keyed by the model pointer (cleaned up after joining workers).

#include "TrackLibrary.h"
#include "SongCatalog.h"
#include "LibraryAnalysisInternal.h"
#include "../analysis/AnalysisInternal.h"
#include "../analysis/BeatGridEditor.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrent>

#include <cmath>
#include <atomic>
#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace gvt {

namespace {

enum Col { ColTitle = 0, ColArtist, ColBpm, ColKey, ColDuration, ColStatus, ColCount };

constexpr int kCacheSchemaVersion = 2;
constexpr int kAnalysisVersion = 2; // gvsf2 structural identity + audio tags

struct Row {
    QString      path;
    TrackDataPtr track;             // null until analyzed
    QString      status = QStringLiteral("Queued");
    double progress = 0.0;
    bool active = false;
    QString error;
    TrackDataPtr profile;
};

struct LibState {
    QThreadPool pool;
    SongCatalog catalog;
    std::vector<Row> rows;
    int total = 0;
    int analyzed = 0;               // GUI thread only
    int generation = 0;             // invalidates in-flight workers on rescan
    int active = 0;
    std::deque<int> pending;
    std::deque<int> urgent;
    std::shared_ptr<std::atomic_bool> cancelled =
        std::make_shared<std::atomic_bool>(false);
    detail::LibraryAnalyzer analyzer;
    LibState() { pool.setMaxThreadCount(4); }
};

std::mutex g_regMutex;
std::mutex g_cacheWriteMutex;
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

QJsonObject readCacheObject(const QString& trackPath)
{
    QFile file(cacheFileFor(trackPath));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

bool cacheMatchesFile(const QJsonObject& object, qint64 mtimeMs)
{
    return !object.isEmpty() &&
           static_cast<qint64>(object.value(QStringLiteral("mtime"))
                                   .toDouble(-1.0)) == mtimeMs;
}

bool hasCurrentAnalysis(const QJsonObject& object)
{
    const bool hasPortableIdentity =
        object.contains(QStringLiteral("camelotKey")) &&
        object.contains(QStringLiteral("isrc")) &&
        object.contains(QStringLiteral("musicBrainzRecording")) &&
        object.value(QStringLiteral("structureFingerprint")).toString()
            .startsWith(QStringLiteral("gvsf2:"));
    if (!hasPortableIdentity) return false;

    // The portable-identity release predates an explicit analysis version.
    // Its complete gvsf2 records are already version 2 and only need the
    // non-destructive cache-schema wrapper added.
    if (!object.contains(QStringLiteral("analysisVersion"))) return true;
    return object.value(QStringLiteral("analysisVersion")).toInt() >=
           kAnalysisVersion;
}

bool hasStoredGrid(const QJsonObject& object)
{
    const double bpm = object.value(QStringLiteral("bpm")).toDouble();
    const double anchor = object.value(QStringLiteral("firstBeatSec")).toDouble();
    return std::isfinite(bpm) && bpm > 0.0 && std::isfinite(anchor);
}

void loadPerformanceState(const QJsonObject& object, TrackData& track)
{
    const QJsonArray cues = object.value(QStringLiteral("hotCues")).toArray();
    for (int i = 0; i < 8 && i < cues.size(); ++i)
        track.hotCues[i] = cues[i].toDouble(-1.0);

    const QJsonArray savedLoops =
        object.value(QStringLiteral("savedLoops")).toArray();
    for (int i = 0; i < 8 && i < savedLoops.size(); ++i) {
        const QJsonObject saved = savedLoops.at(i).toObject();
        SavedLoopSlot slot;
        slot.startSec = saved.value(QStringLiteral("startSec")).toDouble(-1.0);
        slot.endSec = saved.value(QStringLiteral("endSec")).toDouble(-1.0);
        slot.label = saved.value(QStringLiteral("label")).toString();
        track.savedLoops[i] = slot.isSet() ? slot : SavedLoopSlot{};
    }
}

void preserveAuthoredState(const QJsonObject& previous, TrackData& analyzed)
{
    const QString recordedSource =
        previous.value(QStringLiteral("beatGridSource")).toString();
    // Even an originally automatic grid may now be the coordinate system of
    // saved transitions. Refresh detector results separately; changing the
    // effective grid requires an explicit user edit, not an analysis upgrade.
    if (hasStoredGrid(previous)) {
        analyzed.bpm = previous.value(QStringLiteral("bpm")).toDouble();
        analyzed.firstBeatSec =
            previous.value(QStringLiteral("firstBeatSec")).toDouble();
        analyzed.beatGridSource = recordedSource.isEmpty()
            ? QStringLiteral("legacy-preserved") : recordedSource;
    }
    loadPerformanceState(previous, analyzed);
}

// Similarity fingerprints deliberately tolerate changes and cannot authorize
// overwriting user work. Require exact audio/asset evidence across file changes.
bool verifyCachedAudio(const QJsonObject& previous, const TrackData& current,
                       qint64 mtimeMs, QString* error)
{
    if (current.decodedAudioSha256.isEmpty() || current.assetSha256.isEmpty()) {
        if (error) *error = QStringLiteral("Could not verify the audio file; saved settings were kept unchanged. Please retry Load.");
        return false;
    }
    if (previous.isEmpty()) return true;
    const QString pcm = previous.value(QStringLiteral("decodedAudioSha256")).toString();
    const QString asset = previous.value(QStringLiteral("assetSha256")).toString();
    if (!pcm.isEmpty()) {
        if (pcm == current.decodedAudioSha256) return true;
    } else if (!asset.isEmpty()) {
        if (asset == current.assetSha256) return true;
    } else if (cacheMatchesFile(previous, mtimeMs)) {
        // Old records lacking exact evidence can be upgraded in place only
        // while their original file timestamp and legacy evidence still agree.
        const QString fingerprint = previous.value(QStringLiteral("fingerprint")).toString();
        const double duration = previous.value(QStringLiteral("durationSec")).toDouble();
        if (!fingerprint.isEmpty() && fingerprint == current.fingerprint &&
            std::abs(duration - current.durationSec) < 1.0 / kSampleRate)
            return true;
    }
    if (error) *error = QStringLiteral(
        "Audio changed or its identity cannot be verified. Your saved BPM, downbeat, "
        "hot cues and loops have been kept unchanged; this file was not loaded. "
        "Restore the original audio, or keep the replacement under a different filename "
        "and review its beat grid before using transitions.");
    return false;
}

bool preserveCacheBytes(const QString& path, const QByteArray& bytes, QString* error)
{
    // Content-addressed, immutable copies avoid duplicate backups on retries.
    const QString dir = cacheDirPath() + QStringLiteral("/preserved");
    const QString backup = dir + QLatin1Char('/') + QFileInfo(path).baseName() +
        QLatin1Char('-') + QString::fromLatin1(QCryptographicHash::hash(
            bytes, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".json");
    QFile existing(backup);
    if (existing.exists()) {
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes) return true;
    } else if (QDir().mkpath(dir)) {
        QSaveFile saved(backup);
        if (saved.open(QIODevice::WriteOnly) && saved.write(bytes) == bytes.size() &&
            saved.commit()) return true;
    }
    if (error) *error = QStringLiteral("Could not back up saved setup; existing cache was not changed: %1").arg(backup);
    return false;
}

bool writeCache(const TrackData& t, qint64 mtimeMs, QString* error = nullptr,
                const TrackData* gridOverride = nullptr,
                const TrackData* performanceOverride = nullptr,
                const QJsonObject* expectedPrevious = nullptr)
{
    std::lock_guard<std::mutex> guard(g_cacheWriteMutex);
    if (error)
        error->clear();
    const QString dirPath = cacheDirPath();
    if (!QDir().mkpath(dirPath)) {
        if (error)
            *error = QStringLiteral("Could not create analysis cache: %1")
                         .arg(dirPath);
        return false;
    }
    const QString cachePath = cacheFileFor(t.filePath);
    QFile previousFile(cachePath);
    const bool existed = previousFile.exists();
    QByteArray previousBytes;
    if (existed) {
        if (!previousFile.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Could not read saved setup; cache was not changed");
            return false;
        }
        previousBytes = previousFile.readAll();
    }
    const QJsonDocument previousDocument = QJsonDocument::fromJson(previousBytes);
    if (existed && !previousDocument.isObject()) {
        if (error) *error = QStringLiteral("Saved setup is unreadable; cache was not changed");
        return false;
    }
    const QJsonObject previous = previousDocument.object();
    if (expectedPrevious && previous != *expectedPrevious) {
        if (error) *error = QStringLiteral("Saved setup changed during analysis. Please retry Load; no settings were overwritten.");
        return false;
    }
    if ((gridOverride || performanceOverride) && !previous.isEmpty() &&
        !cacheMatchesFile(previous, mtimeMs)) {
        if (error) *error = QStringLiteral("Audio file changed on disk. Reload it before saving the grid or cues; existing settings were kept.");
        return false;
    }
    QJsonObject o = previous; // retain optional future local fields
    o[QStringLiteral("cacheVersion")] = kCacheSchemaVersion;
    o[QStringLiteral("analysisVersion")] = kAnalysisVersion;
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
    o[QStringLiteral("analyzedBpm")] = t.analyzedBpm;
    o[QStringLiteral("analyzedFirstBeatSec")] = t.analyzedFirstBeatSec;
    o[QStringLiteral("beatGridSource")] = gridOverride
        ? QStringLiteral("user")
        : (t.beatGridSource.isEmpty() ? QStringLiteral("analysis")
                                      : t.beatGridSource);
    o[QStringLiteral("fingerprint")]  = t.fingerprint;
    o[QStringLiteral("structureFingerprint")] = t.structureFingerprint;
    o[QStringLiteral("assetSha256")] = t.assetSha256;
    o[QStringLiteral("decodedAudioSha256")] = t.decodedAudioSha256;
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

    const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Indented);
    if (json == previousBytes) return true;
    if (!previousBytes.isEmpty() && !preserveCacheBytes(cachePath, previousBytes, error))
        return false;
    QSaveFile f(cachePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Could not open analysis cache: %1")
                         .arg(f.errorString());
        return false;
    }
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
TrackDataPtr loadFromCache(const QString& path, qint64 mtimeMs,
                           const QJsonObject& o, QString* error,
                           const detail::AnalysisProgress& progress)
{
    if (!hasCurrentAnalysis(o))
        return nullptr;

    auto t = std::make_shared<TrackData>();
    t->filePath = path;
    if (!detail::decodeAudioStereo48k(path, t->pcm, error, [&](double p) {
            if (progress) progress(.6 * p, QStringLiteral("Decoding"));
        })) return nullptr;
    t->durationSec = double(t->frameCount()) / kSampleRate;
    t->decodedAudioSha256 = detail::decodedAudioHash(t->pcm, [&](double p) {
        if (progress) progress(.6 + .05 * p, QStringLiteral("Checking audio"));
    });
    t->assetSha256 = detail::assetFileHash(path, [&](double p) {
        if (progress) progress(.65 + .05 * p, QStringLiteral("Checking audio"));
    });
    t->fingerprint = computeFingerprint(t->pcm.data(), t->frameCount());
    if (!verifyCachedAudio(o, *t, mtimeMs, error)) return nullptr;
    t->title        = o.value(QStringLiteral("title")).toString();
    t->artist       = o.value(QStringLiteral("artist")).toString();
    t->album        = o.value(QStringLiteral("album")).toString();
    t->isrc = o.value(QStringLiteral("isrc")).toString();
    t->musicBrainzRecording =
        o.value(QStringLiteral("musicBrainzRecording")).toString();
    t->bpm          = o.value(QStringLiteral("bpm")).toDouble();
    t->firstBeatSec = o.value(QStringLiteral("firstBeatSec")).toDouble();
    t->analyzedBpm = o.value(QStringLiteral("analyzedBpm"))
                         .toDouble(t->bpm);
    t->analyzedFirstBeatSec =
        o.value(QStringLiteral("analyzedFirstBeatSec"))
            .toDouble(t->firstBeatSec);
    t->beatGridSource =
        o.value(QStringLiteral("beatGridSource")).toString();
    if (t->beatGridSource.isEmpty())
        t->beatGridSource = QStringLiteral("legacy-preserved");
    t->structureFingerprint =
        o.value(QStringLiteral("structureFingerprint")).toString();
    t->audibleDurationSec =
        o.value(QStringLiteral("audibleDurationSec")).toDouble();
    t->songId = o.value(QStringLiteral("songId")).toString();
    t->camelotKey   = o.value(QStringLiteral("camelotKey")).toString();
    t->keyName      = o.value(QStringLiteral("keyName")).toString();
    t->durationSec  = (double)t->frameCount() / (double)kSampleRate;
    loadPerformanceState(o, *t);
    if (!cacheMatchesFile(o, mtimeMs) ||
        t->assetSha256 != o.value(QStringLiteral("assetSha256")).toString())
        detail::readTags(path, t->title, t->artist, t->album, t->isrc, t->musicBrainzRecording);
    if (t->title.isEmpty()) t->title = QFileInfo(path).completeBaseName();
    t->overviewPeaks = detail::computeOverviewPeaks(t->pcm, [&](double p) {
        if (progress) progress(.7 + .05 * p, QStringLiteral("Waveform"));
    });
    detail::computeBandOverviews(t->pcm, t->overviewLow, t->overviewMid, t->overviewHigh,
        [&](double p) {
            if (progress) progress(.75 + .24 * p, QStringLiteral("Waveform"));
        });
    if (progress) progress(.99, QStringLiteral("Finishing"));
    return t;
}

TrackDataPtr analyzeWithCache(const QString& path, QString* error,
                             const detail::AnalysisProgress& progress)
{
    const qint64 mtimeMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    const qint64 fileSize = QFileInfo(path).size();
    const QJsonObject previous = readCacheObject(path);
    if (error) error->clear();
    if (QFileInfo::exists(cacheFileFor(path)) && previous.isEmpty()) {
        if (error) *error = QStringLiteral("Saved setup is unreadable; kept unchanged. Restore its cache backup before loading this song.");
        return nullptr;
    }
    const auto unchangedFile = [&] {
        const QFileInfo now(path);
        if (now.lastModified().toMSecsSinceEpoch() == mtimeMs && now.size() == fileSize)
            return true;
        if (error) *error = QStringLiteral("Audio changed during analysis. Please retry Load; saved settings were kept.");
        return false;
    };
    if (TrackDataPtr cached = loadFromCache(path, mtimeMs, previous, error, progress)) {
        if (!unchangedFile()) return nullptr;
        if (previous.value(QStringLiteral("cacheVersion")).toInt() <
                kCacheSchemaVersion ||
            !previous.contains(QStringLiteral("analyzedBpm")) ||
            !previous.contains(QStringLiteral("beatGridSource")) ||
            previous.value(QStringLiteral("decodedAudioSha256")).toString() != cached->decodedAudioSha256 ||
            !cacheMatchesFile(previous, mtimeMs) ||
            previous.value(QStringLiteral("assetSha256")).toString() != cached->assetSha256) {
            // Schema-only migrations rewrite the already-loaded record; they
            // never re-run analysis or discard effective/performance state.
            if (!writeCache(*cached, mtimeMs, error, nullptr, nullptr, &previous)) return nullptr;
        }
        return cached;
    }
    if (error && !error->isEmpty()) return nullptr; // never re-analyze around an identity refusal
    TrackDataPtr t = detail::loadAndAnalyzeWithProgress(path, error, progress);
    if (t) {
        if (!unchangedFile() || !verifyCachedAudio(previous, *t, mtimeMs, error)) return nullptr;
        preserveAuthoredState(previous, *t);
        if (!writeCache(*t, mtimeMs, error, nullptr, nullptr, &previous)) return nullptr;
    }
    return t;
}

QString formatDuration(double sec)
{
    const int s = (int)std::lround(sec);
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString naturalKeySortValue(const QString& key)
{
    static const QRegularExpression camelot(
        QStringLiteral("^\\s*(\\d{1,2})\\s*([AaBb])\\s*$"));
    const QRegularExpressionMatch match = camelot.match(key);
    if (match.hasMatch()) {
        return QStringLiteral("0:%1:%2")
            .arg(match.captured(1).toInt(), 2, 10, QLatin1Char('0'))
            .arg(match.captured(2).toUpper());
    }
    // Unknown/non-Camelot values remain stable and case-insensitive, after
    // valid numbered Camelot keys.
    return key.trimmed().isEmpty()
               ? QStringLiteral("2:")
               : QStringLiteral("1:") + key.trimmed().toCaseFolded();
}

} // namespace

TrackLibrary::TrackLibrary(QObject* parent) : QAbstractTableModel(parent)
{
    state(this); // create per-instance state
}

TrackLibrary::~TrackLibrary()
{
    auto st = state(this);
    st->cancelled->store(true);
    // Workers only post queued callbacks; they never wait on the GUI. Join
    // before QObject teardown, so neither callbacks nor pool destruction can
    // race a worker's access to this object.
    st->pool.waitForDone();
    std::lock_guard<std::mutex> lk(g_regMutex);
    g_registry.erase(this);
}

void detail::setLibraryAnalyzerForTesting(TrackLibrary& library, LibraryAnalyzer analyzer)
{
    state(&library)->analyzer = std::move(analyzer);
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

    st->cancelled->store(true);
    st->cancelled = std::make_shared<std::atomic_bool>(false);
    st->generation++;
    st->pending.clear();
    st->urgent.clear();

    beginResetModel();
    st->rows.clear();
    st->rows.reserve((size_t)files.size());
    for (const QString& f : files) {
        st->pending.push_back(static_cast<int>(st->rows.size()));
        st->rows.push_back(Row{f});
        st->rows.back().profile = st->catalog.assetProfile(f);
    }
    st->total = (int)files.size();
    st->analyzed = 0;
    endResetModel();
    emit scanProgress(0, st->total);

    dispatchAnalysis();
}

bool TrackLibrary::prioritizeAnalysis(int row)
{
    auto st = state(this);
    if (row < 0 || row >= static_cast<int>(st->rows.size())) return false;
    auto& item = st->rows[row];
    if (item.track || item.active) return true;
    if (!item.error.isEmpty()) {
        --st->analyzed;
        item.error.clear();
        item.progress = 0;
        emit scanProgress(st->analyzed, st->total);
    }
    std::erase(st->pending, row);
    std::erase(st->urgent, row);
    st->urgent.push_front(row);
    item.status = tr("Queued · priority");
    emit dataChanged(index(row, ColStatus), index(row, ColStatus));
    dispatchAnalysis();
    return true;
}

void TrackLibrary::dispatchAnalysis()
{
    auto st = state(this);
    QPointer<TrackLibrary> self(this);
    // Three background jobs cannot occupy the interactive fourth slot.
    // Never submit the whole library to QThreadPool's opaque FIFO.
    while (st->active < 4 && (!st->urgent.empty() ||
                            (st->active < 3 && !st->pending.empty()))) {
        auto& queue = st->urgent.empty() ? st->pending : st->urgent;
        const int i = queue.front();
        queue.pop_front();
        const int gen = st->generation;
        const auto cancelled = st->cancelled;
        st->rows[i].active = true;
        st->rows[i].status = tr("Starting");
        ++st->active;
        emit dataChanged(index(i, ColStatus), index(i, ColStatus));
        const QString path = st->rows[(size_t)i].path;
        const auto analyzer = st->analyzer ? st->analyzer : analyzeWithCache;
        auto task = [st, self, gen, i, path, cancelled, analyzer] {
            QString err;
            TrackDataPtr t;
            struct Cancelled {};
            QElapsedTimer throttle;
            throttle.start();
            QString lastStage;
            const auto progress = [&](double fraction, const QString& stage) {
                if (cancelled->load()) throw Cancelled{};
                if (stage == lastStage && throttle.elapsed() < 100) return;
                lastStage = stage;
                throttle.restart();
                QMetaObject::invokeMethod(self, [st, self, gen, i, fraction, stage] {
                    if (!self || gen != st->generation || !st->rows[i].active) return;
                    auto& row = st->rows[i];
                    if (std::isfinite(fraction))
                        row.progress = std::max(row.progress, std::clamp(fraction, 0.0, .99));
                    row.status = stage;
                    emit self->dataChanged(self->index(i, ColStatus), self->index(i, ColStatus));
                }, Qt::QueuedConnection);
            };
            try {
                progress(0, QStringLiteral("Starting"));
                t = analyzer(path, &err, progress);
            } catch (const Cancelled&) {
            } catch (const std::exception& exception) {
                err = QString::fromUtf8(exception.what());
            } catch (...) {
                err = QStringLiteral("Unexpected analysis failure");
            }
            TrackLibrary* obj = self.data();
            if (!obj) return;
            QMetaObject::invokeMethod(obj, [st, self, gen, i, t, err] {
                TrackLibrary* m = self.data();
                --st->active;
                if (!m) return;
                if (gen != st->generation) {
                    m->dispatchAnalysis();
                    return;
                }
                if (t) {
                    t->songId = st->catalog.registerAsset(*t);
                    t->canonicalBeatOffset =
                        st->catalog.canonicalBeatOffsetForAsset(t->filePath);
                }
                auto& row = st->rows[i];
                row.track = t;
                row.active = false;
                row.progress = t ? 1.0 : row.progress;
                row.error = t ? QString() : (err.isEmpty() ? tr("Could not analyze audio") : err);
                row.status = t ? tr("ready") : tr("Needs attention");
                st->analyzed++;
                emit m->dataChanged(m->index(i, 0), m->index(i, ColCount - 1));
                emit m->trackReady(i);
                emit m->scanProgress(st->analyzed, st->total);
                m->dispatchAnalysis();
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

TrackDataPtr TrackLibrary::profileAt(int row) const
{
    const auto st = state(this);
    if (row < 0 || row >= static_cast<int>(st->rows.size())) return {};
    const auto& item = st->rows[row];
    return item.track ? item.track : item.profile;
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
    emit transitionGraphChanged();
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
    libraryRow.track->beatGridSource = QStringLiteral("user");
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

    if (role == AnalysisProgressRole) return r.progress;
    if (role == AnalysisActiveRole) return r.active;
    if (role == AnalysisErrorRole) return r.error;
    if (role == Qt::ToolTipRole)
        return r.error.isEmpty() ? r.path + QLatin1Char('\n') + r.status +
            (r.track ? QString() : tr("\nLoad or double-click to prioritize this song."
                                     " Progress measures analysis work, not time remaining."))
                                : r.path + QLatin1Char('\n') + r.error;

    if (role == Qt::TextAlignmentRole) {
        if (idx.column() == ColBpm || idx.column() == ColKey || idx.column() == ColDuration)
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }
    if (role == Qt::UserRole) {
        switch (idx.column()) {
        case ColTitle:
            return r.track && !r.track->title.isEmpty()
                       ? r.track->title
                       : QFileInfo(r.path).completeBaseName();
        case ColArtist:
            return r.track ? r.track->artist : QString();
        case ColBpm:
            return (r.track && r.track->bpm > 0.0)
                       ? QVariant(r.track->bpm) : QVariant();
        case ColKey:
            return naturalKeySortValue(
                r.track ? r.track->camelotKey : QString());
        case ColDuration:
            return r.track ? QVariant(r.track->durationSec) : QVariant();
        case ColStatus:
            return r.status;
        }
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
        return r.progress >= 1.0 ? r.status
            : QStringLiteral("%1 · %2%").arg(r.status).arg(qRound(r.progress * 100));
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

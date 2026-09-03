#include "library/TrackLibrary.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        ++failures; \
    } \
} while (0)

bool writeWavNamedMp3(const QString& path)
{
    constexpr quint32 sampleRate = 48000;
    constexpr quint16 channels = 2;
    constexpr quint16 bits = 16;
    constexpr quint32 frames = sampleRate * 2;
    constexpr quint32 dataBytes = frames * channels * (bits / 8);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    file.write("RIFF", 4);
    out << quint32(36 + dataBytes);
    file.write("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << channels << sampleRate;
    out << quint32(sampleRate * channels * (bits / 8));
    out << quint16(channels * (bits / 8)) << bits;
    file.write("data", 4);
    out << dataBytes;
    QByteArray silence(static_cast<qsizetype>(dataBytes), '\0');
    return file.write(silence) == silence.size();
}

bool scanAndWait(gvt::TrackLibrary& library, const QString& directory)
{
    bool ready = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&library, &gvt::TrackLibrary::trackReady, &loop,
                     [&](int row) {
                         if (row == 0) {
                             ready = true;
                             loop.quit();
                         }
                     });
    timeout.start(10000);
    library.scanFolder(directory);
    loop.exec();
    return ready;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    if (!temporary.isValid())
        return 1;

    const QString musicDir = temporary.path() + QStringLiteral("/music");
    const QString cacheDir = temporary.path() + QStringLiteral("/cache");
    CHECK(QDir().mkpath(musicDir));
    qputenv("GRAVITINO_CACHE_DIR", cacheDir.toUtf8());
    CHECK(writeWavNamedMp3(musicDir + QStringLiteral("/grid-test.mp3")));

    gvt::TrackLibrary library;
    CHECK(scanAndWait(library, musicDir));
    CHECK(library.trackCount() == 1);
    gvt::TrackDataPtr track = library.trackAt(0);
    CHECK(track != nullptr);
    if (!track)
        return 1;

    bool bpmCellChanged = false;
    QObject::connect(&library, &QAbstractItemModel::dataChanged, &library,
                     [&](const QModelIndex& first, const QModelIndex& last) {
                         bpmCellChanged = first.row() == 0 && last.row() == 0 &&
                                          first.column() == 2 && last.column() == 2;
                     });
    track->bpm = 126.75;
    track->firstBeatSec = 0.321;
    QString error;
    CHECK(library.persistBeatGrid(*track, &error));
    CHECK(error.isEmpty());
    CHECK(bpmCellChanged);

    // A rescan destroys the in-memory row and reloads from the JSON cache.
    CHECK(scanAndWait(library, musicDir));
    gvt::TrackDataPtr reloaded = library.trackAt(0);
    CHECK(reloaded != nullptr);
    if (!reloaded)
        return 1;
    CHECK(std::fabs(reloaded->bpm - 126.75) < 1e-9);
    CHECK(std::fabs(reloaded->firstBeatSec - 0.321) < 1e-9);

    // A deck can retain an older TrackData instance across a library rescan.
    // Persisting its grid must merge only BPM/anchor into the current row and
    // must not resurrect stale title or hot-cue metadata.
    reloaded->title = QStringLiteral("Current metadata");
    reloaded->hotCues[0] = 0.777;
    gvt::TrackData staleDeckCopy;
    staleDeckCopy.filePath = reloaded->filePath;
    staleDeckCopy.title = QStringLiteral("Stale metadata");
    staleDeckCopy.bpm = 132.25;
    staleDeckCopy.firstBeatSec = 0.456;
    staleDeckCopy.hotCues[0] = 1.5;
    CHECK(library.persistBeatGrid(staleDeckCopy, &error));
    CHECK(scanAndWait(library, musicDir));
    reloaded = library.trackAt(0);
    CHECK(reloaded != nullptr);
    if (!reloaded)
        return 1;
    CHECK(reloaded->title == QStringLiteral("Current metadata"));
    CHECK(std::fabs(reloaded->hotCues[0] - 0.777) < 1e-9);
    CHECK(std::fabs(reloaded->bpm - 132.25) < 1e-9);
    CHECK(std::fabs(reloaded->firstBeatSec - 0.456) < 1e-9);

    // Performance metadata is the inverse merge: author hot cues and saved
    // loops from a possibly stale loaded deck, while retaining the library's
    // newer analysis/grid/descriptive fields.
    gvt::TrackData stalePerformance;
    stalePerformance.filePath = reloaded->filePath;
    stalePerformance.title = QStringLiteral("Must not replace current title");
    stalePerformance.bpm = 90.0;
    stalePerformance.firstBeatSec = 9.0;
    stalePerformance.hotCues[2] = 0.625;
    stalePerformance.savedLoops[3].startSec = 0.75;
    stalePerformance.savedLoops[3].endSec = 1.75;
    stalePerformance.savedLoops[3].label = QStringLiteral("Chorus");
    CHECK(library.persistPerformanceMetadata(stalePerformance, &error));
    CHECK(error.isEmpty());
    CHECK(scanAndWait(library, musicDir));
    reloaded = library.trackAt(0);
    CHECK(reloaded != nullptr);
    if (!reloaded)
        return 1;
    CHECK(reloaded->title == QStringLiteral("Current metadata"));
    CHECK(std::fabs(reloaded->bpm - 132.25) < 1e-9);
    CHECK(std::fabs(reloaded->firstBeatSec - 0.456) < 1e-9);
    CHECK(std::fabs(reloaded->hotCues[2] - 0.625) < 1e-9);
    CHECK(reloaded->savedLoops[3].isSet());
    CHECK(std::fabs(reloaded->savedLoops[3].startSec - 0.75) < 1e-9);
    CHECK(std::fabs(reloaded->savedLoops[3].endSec - 1.75) < 1e-9);
    CHECK(reloaded->savedLoops[3].label == QStringLiteral("Chorus"));

    // Regression: adding new derived-analysis fields used to turn an older
    // cache into a miss and then overwrite its corrected grid, hot cues, and
    // saved loops. Simulate that old record and require the upgrade to merge
    // the newly derived fields around the authored state.
    const QStringList cacheFiles = QDir(cacheDir).entryList(
        {QStringLiteral("*.json")}, QDir::Files);
    CHECK(cacheFiles.size() == 1);
    const QString cachePath = cacheDir + QLatin1Char('/') + cacheFiles.front();
    QFile oldCacheFile(cachePath);
    CHECK(oldCacheFile.open(QIODevice::ReadOnly));
    QJsonObject oldCache = QJsonDocument::fromJson(
        oldCacheFile.readAll()).object();
    oldCacheFile.close();
    oldCache.remove(QStringLiteral("cacheVersion"));
    oldCache.remove(QStringLiteral("analysisVersion"));
    oldCache.remove(QStringLiteral("analyzedBpm"));
    oldCache.remove(QStringLiteral("analyzedFirstBeatSec"));
    oldCache.remove(QStringLiteral("beatGridSource"));
    oldCache.remove(QStringLiteral("isrc"));
    oldCache.remove(QStringLiteral("musicBrainzRecording"));
    oldCache.remove(QStringLiteral("structureFingerprint"));
    oldCache.remove(QStringLiteral("assetSha256"));
    oldCache.remove(QStringLiteral("audibleDurationSec"));
    oldCache.insert(QStringLiteral("bpm"), 123.45);
    oldCache.insert(QStringLiteral("firstBeatSec"), 0.234);
    QJsonArray legacyCues = oldCache.value(
        QStringLiteral("hotCues")).toArray();
    legacyCues[5] = 0.9;
    oldCache.insert(QStringLiteral("hotCues"), legacyCues);
    QJsonArray legacyLoops = oldCache.value(
        QStringLiteral("savedLoops")).toArray();
    QJsonObject legacyLoop;
    legacyLoop.insert(QStringLiteral("startSec"), 0.4);
    legacyLoop.insert(QStringLiteral("endSec"), 1.4);
    legacyLoop.insert(QStringLiteral("label"), QStringLiteral("Protected"));
    legacyLoops[6] = legacyLoop;
    oldCache.insert(QStringLiteral("savedLoops"), legacyLoops);
    CHECK(oldCacheFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray oldBytes = QJsonDocument(oldCache).toJson();
    CHECK(oldCacheFile.write(oldBytes) == oldBytes.size());
    oldCacheFile.close();

    CHECK(scanAndWait(library, musicDir));
    reloaded = library.trackAt(0);
    CHECK(reloaded != nullptr);
    if (!reloaded)
        return 1;
    CHECK(std::fabs(reloaded->bpm - 123.45) < 1e-9);
    CHECK(std::fabs(reloaded->firstBeatSec - 0.234) < 1e-9);
    CHECK(reloaded->beatGridSource == QStringLiteral("legacy-preserved"));
    CHECK(std::fabs(reloaded->hotCues[5] - 0.9) < 1e-9);
    CHECK(reloaded->savedLoops[6].isSet());
    CHECK(reloaded->savedLoops[6].label == QStringLiteral("Protected"));

    CHECK(oldCacheFile.open(QIODevice::ReadOnly));
    const QJsonObject upgradedCache = QJsonDocument::fromJson(
        oldCacheFile.readAll()).object();
    CHECK(upgradedCache.value(QStringLiteral("cacheVersion")).toInt() == 2);
    CHECK(upgradedCache.value(QStringLiteral("analysisVersion")).toInt() == 2);
    CHECK(upgradedCache.value(QStringLiteral("beatGridSource")).toString() ==
          QStringLiteral("legacy-preserved"));
    CHECK(upgradedCache.value(QStringLiteral("structureFingerprint")).toString()
              .startsWith(QStringLiteral("gvsf2:")));
    oldCacheFile.close();

    // A future derived-analysis version must likewise keep an explicitly
    // user-approved grid. Performance metadata remains independent of the
    // grid source and survives either kind of upgrade.
    QJsonObject futureCache = upgradedCache;
    futureCache.insert(QStringLiteral("analysisVersion"), 1);
    futureCache.insert(QStringLiteral("bpm"), 129.5);
    futureCache.insert(QStringLiteral("firstBeatSec"), 0.345);
    futureCache.insert(QStringLiteral("beatGridSource"),
                       QStringLiteral("user"));
    QJsonArray futureCues = futureCache.value(
        QStringLiteral("hotCues")).toArray();
    futureCues[7] = 1.125;
    futureCache.insert(QStringLiteral("hotCues"), futureCues);
    CHECK(oldCacheFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray futureBytes = QJsonDocument(futureCache).toJson();
    CHECK(oldCacheFile.write(futureBytes) == futureBytes.size());
    oldCacheFile.close();

    CHECK(scanAndWait(library, musicDir));
    reloaded = library.trackAt(0);
    CHECK(reloaded != nullptr);
    if (!reloaded)
        return 1;
    CHECK(std::fabs(reloaded->bpm - 129.5) < 1e-9);
    CHECK(std::fabs(reloaded->firstBeatSec - 0.345) < 1e-9);
    CHECK(reloaded->beatGridSource == QStringLiteral("user"));
    CHECK(std::fabs(reloaded->hotCues[7] - 1.125) < 1e-9);
    CHECK(reloaded->savedLoops[6].isSet());

    gvt::TrackData invalid;
    invalid.filePath = reloaded->filePath;
    invalid.bpm = 0.0;
    CHECK(!library.persistBeatGrid(invalid, &error));
    CHECK(!error.isEmpty());

    qunsetenv("GRAVITINO_CACHE_DIR");
    if (failures != 0)
        return 1;
    std::printf("test_beatgrid_persistence: corrected grid survived rescan\n");
    return 0;
}

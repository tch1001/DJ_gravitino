// Song catalog tests — multiple assets share one arrangement while reverse
// transition edges remain a rebuildable local index.

#include "library/SongCatalog.h"
#include "analysis/TrackData.h"
#include "transitions/Transition.h"

#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)
}

int main()
{
    using namespace gvt;
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const QString catalogPath = directory.filePath(QStringLiteral("catalog.json"));
    SongCatalog catalog(catalogPath);

    TrackData mp3;
    mp3.filePath = directory.filePath(QStringLiteral("song.mp3"));
    mp3.title = QStringLiteral("Song");
    mp3.artist = QStringLiteral("Artist");
    mp3.bpm = 128.0;
    mp3.durationSec = 180.0;
    mp3.audibleDurationSec = 179.5;
    mp3.fingerprint = QStringLiteral("gvfp1:mp3");
    mp3.structureFingerprint =
        QStringLiteral("gvsf1:0123456789abcdef0123456789abcdef");
    mp3.assetSha256 = QStringLiteral("bytes-mp3");
    { QFile file(mp3.filePath); CHECK(file.open(QIODevice::WriteOnly)); }

    TrackData flac = mp3;
    flac.filePath = directory.filePath(QStringLiteral("song.flac"));
    flac.fingerprint = QStringLiteral("gvfp1:flac");
    flac.assetSha256 = QStringLiteral("bytes-flac");
    { QFile file(flac.filePath); CHECK(file.open(QIODevice::WriteOnly)); }

    QString error;
    const QString songId = catalog.registerAsset(mp3, &error);
    CHECK(!songId.isEmpty());
    const QString flacSongId = catalog.registerAsset(flac, &error);
    CHECK(flacSongId == songId);
    CHECK(catalog.assetsForSong(songId).size() == 2);

    CHECK(catalog.confirmBinding(flac.filePath, songId, 0.25, &error));
    CHECK(catalog.canonicalBeatOffsetForAsset(flac.filePath) == 0.25);

    GvtFile transition;
    transition.id = QStringLiteral("transition-id");
    transition.filePath = directory.filePath(QStringLiteral("mix.transition"));
    transition.sourceFormat = TransitionSourceFormat::PortableYaml;
    transition.from.title = mp3.title;
    transition.from.artist = mp3.artist;
    transition.from.bpm = mp3.bpm;
    transition.from.durationSec = mp3.durationSec;
    transition.from.fingerprints.push_back(
        {QStringLiteral("gravitino-structure-1"),
         mp3.structureFingerprint, {}});
    transition.to = transition.from;
    catalog.rebuildTransitionGraph({transition});
    const auto links = catalog.transitionsForSong(songId);
    CHECK(links.size() == 2);
    CHECK(links[0].transitionId == transition.id);
    CHECK(links[0].outgoing != links[1].outgoing);

    TrackData manual = mp3;
    manual.filePath = directory.filePath(QStringLiteral("manual.wav"));
    manual.structureFingerprint = QStringLiteral(
        "gvsf1:ffffffffffffffffffffffffffffffff");
    { QFile file(manual.filePath); CHECK(file.open(QIODevice::WriteOnly)); }
    const QString manualSong = catalog.registerAsset(manual, &error);
    CHECK(!manualSong.isEmpty());
    CHECK(manualSong != songId);
    CHECK(catalog.confirmEndpointBinding(
        transition.id, true, manualSong, &error));
    CHECK(catalog.songIdForEndpoint(transition.id, true) == manualSong);
    catalog.rebuildTransitionGraph({transition});
    CHECK(catalog.transitionsForSong(manualSong).size() == 1);
    CHECK(catalog.transitionsForSong(manualSong)[0].outgoing);

    CHECK(QFile::remove(flac.filePath));
    CHECK(catalog.assetsForSong(songId).size() == 1);

    SongCatalog reloaded(catalogPath);
    CHECK(reloaded.songIdForAsset(mp3.filePath) == songId);
    CHECK(reloaded.assetsForSong(songId).size() == 1);
    CHECK(reloaded.songIdForEndpoint(transition.id, true) == manualSong);

    if (failures) return 1;
    std::printf("test_song_catalog: asset grouping and reverse graph passed\n");
    return 0;
}

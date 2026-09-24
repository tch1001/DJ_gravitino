// Protect authored song setup across timestamp/tag churn, and refuse to bind
// old beat coordinates silently to different audio. All files are temporary.
#include "library/TrackLibrary.h"
#include "analysis/AnalysisInternal.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <taglib/wavfile.h>
#include <taglib/tag.h>
#include <taglib/fileref.h>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; \
} } while (0)

QByteArray read(const QString& path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write(bytes) == bytes.size());
}
void setTime(const QString& path, const QDateTime& time)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadWrite));
    CHECK(file.setFileTime(time, QFileDevice::FileModificationTime));
}
void makeAudio(const QString& path)
{
    constexpr quint32 frames = 48000 * 35;
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    file.write("RIFF", 4); out << quint32(36 + frames * 4);
    file.write("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(2) << quint32(48000)
        << quint32(192000) << quint16(4) << quint16(16);
    file.write("data", 4); out << quint32(frames * 4);
    const QByteArray silence(frames * 4, '\0');
    CHECK(file.write(silence) == silence.size());
}

struct Fixture {
    QTemporaryDir root;
    QString music, audio, cache, catalog, record;
    gvt::TrackLibrary library;
    static QObject* configure(const QTemporaryDir& root)
    {
        qputenv("GRAVITINO_CACHE_DIR", root.filePath("cache").toUtf8());
        qputenv("GRAVITINO_CATALOG_PATH", root.filePath("catalog.json").toUtf8());
        return nullptr;
    }
    Fixture() : library(configure(root))
    {
        CHECK(root.isValid());
        music = root.filePath("music");
        audio = music + "/low.wav";
        cache = root.filePath("cache");
        catalog = root.filePath("catalog.json");
        CHECK(QDir().mkpath(music));
        qputenv("GRAVITINO_CACHE_DIR", cache.toUtf8());
        qputenv("GRAVITINO_CATALOG_PATH", catalog.toUtf8());
        makeAudio(audio);
    }
    bool scan()
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool done = false;
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&library, &gvt::TrackLibrary::trackReady, &loop, [&](int) {
            done = true;
            loop.quit();
        });
        library.scanFolder(music);
        timeout.start(10000);
        loop.exec();
        CHECK(done);
        return library.trackAt(0) != nullptr;
    }
    void seed()
    {
        CHECK(scan());
        auto track = library.trackAt(0);
        if (!track) return;
        track->bpm = 127.99090909090886;
        track->firstBeatSec = 16.227126792257145;
        track->hotCues[0] = 17.123;
        track->savedLoops[2] = {18.0, 22.0, QStringLiteral("Protected loop")};
        QString error;
        CHECK(library.persistBeatGrid(*track, &error));
        CHECK(library.persistPerformanceMetadata(*track, &error));
        const auto files = QDir(cache).entryList({"*.json"}, QDir::Files);
        CHECK(files.size() == 1);
        if (!files.isEmpty()) record = cache + '/' + files.front();
    }
    void checkSetup()
    {
        auto track = library.trackAt(0);
        CHECK(track);
        if (!track) return;
        CHECK(std::abs(track->bpm - 127.99090909090886) < 1e-10);
        CHECK(std::abs(track->firstBeatSec - 16.227126792257145) < 1e-10);
        CHECK(track->hotCues[0] == 17.123);
        CHECK(track->savedLoops[2].startSec == 18.0);
        CHECK(track->savedLoops[2].endSec == 22.0);
        CHECK(track->savedLoops[2].label == "Protected loop");
        CHECK(track->beatGridSource == "user");
        // Both formats retain exactly the same cue/loop mapping after reloading.
        for (auto format : {gvt::TransitionSourceFormat::PortableYaml,
                            gvt::TransitionSourceFormat::LegacyGvt}) {
            gvt::GvtFile transition;
            transition.sourceFormat = format;
            CHECK(std::abs(gvt::transitionSecAtBeat(transition, *track, 0) - 16.227126792257145) < 1e-10);
            CHECK(std::abs(gvt::transitionSecAtBeat(transition, *track, 32.125) -
                (16.227126792257145 + 32.125 * 60 / track->bpm)) < 1e-10);
        }
    }
    void checkBackup(const QByteArray& expected)
    {
        QDir dir(cache + "/preserved");
        bool found = false;
        for (const auto& name : dir.entryList({"*.json"}, QDir::Files))
            if (read(dir.filePath(name)) == expected) found = true;
        CHECK(found);
    }
};

void timestamp_changes_keep_the_entire_saved_setup()
{
    Fixture f;
    f.seed();
    const auto originalAudio = read(f.audio);
    const auto originalCache = read(f.record);
    const auto previousHash = f.library.trackAt(0)->decodedAudioSha256;
    setTime(f.audio, QFileInfo(f.audio).lastModified().addSecs(10));
    CHECK(f.scan());
    f.checkSetup();
    CHECK(read(f.audio) == originalAudio);
    CHECK(f.library.trackAt(0)->decodedAudioSha256 == previousHash);
    f.checkBackup(originalCache);
    const auto after = read(f.record);
    CHECK(f.scan());
    CHECK(read(f.record) == after); // ordinary launches do not keep rewriting
}

void tag_edits_refresh_metadata_without_regridding()
{
    Fixture f;
    f.seed();
    const auto oldPcm = f.library.trackAt(0)->pcm;
    const auto oldHash = f.library.trackAt(0)->decodedAudioSha256;
    const auto oldAsset = f.library.trackAt(0)->assetSha256;
    const auto oldCache = read(f.record);
    {
        TagLib::RIFF::WAV::File file(f.audio.toUtf8().constData());
        file.tag()->setTitle("New title only");
        file.tag()->setArtist("New artist only");
        CHECK(file.save());
    }
    setTime(f.audio, QFileInfo(f.audio).lastModified().addSecs(10));
    CHECK(f.scan());
    f.checkSetup();
    auto track = f.library.trackAt(0);
    CHECK(track->title == "New title only");
    CHECK(track->artist == "New artist only");
    CHECK(track->pcm == oldPcm);
    CHECK(track->decodedAudioSha256 == oldHash);
    CHECK(track->assetSha256 != oldAsset);
    f.checkBackup(oldCache);
}

void changed_audio_is_refused_even_when_the_timestamp_and_short_fingerprint_match()
{
    Fixture f;
    f.seed();
    const auto originalAudio = read(f.audio);
    const auto originalTime = QFileInfo(f.audio).lastModified();
    CHECK(f.scan()); // bring catalog in sync with the authored setup
    const auto oldCache = read(f.record);
    const auto oldCatalog = read(f.catalog);
    const auto shortFingerprint = f.library.trackAt(0)->fingerprint;
    QByteArray changed = originalAudio;
    changed[44 + 34 * 48000 * 4] = 100; // after gvfp1's first 30 seconds
    write(f.audio, changed);
    setTime(f.audio, originalTime);
    QString error;
    const auto candidate = gvt::loadAndAnalyzeTrack(f.audio, &error);
    CHECK(candidate && candidate->fingerprint == shortFingerprint);
    CHECK(!f.scan());
    CHECK(f.library.index(0, 5).data(gvt::TrackLibrary::AnalysisErrorRole).toString().contains("kept unchanged"));
    CHECK(read(f.record) == oldCache);
    CHECK(read(f.catalog) == oldCatalog);
    CHECK(!f.scan()); // repeated retries cannot overwrite the only old evidence
    CHECK(read(f.record) == oldCache);
    write(f.audio, originalAudio);
    setTime(f.audio, originalTime);
    CHECK(f.scan());
    f.checkSetup();
}

void mp3_tag_edits_keep_the_entire_saved_setup()
{
    const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        std::printf("MP3 cache-tag check skipped: ffmpeg unavailable\n");
        return;
    }
    Fixture f;
    CHECK(QFile::rename(f.audio, f.root.filePath("unused.wav")));
    f.audio = f.music + "/low.mp3";
    QProcess encoder;
    encoder.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i",
        "sine=frequency=440:duration=35:sample_rate=44100", "-af",
        "afade=t=out:st=34.8:d=0.1", "-ac", "2", "-q:a", "4", f.audio});
    CHECK(encoder.waitForFinished(30000));
    CHECK(encoder.exitCode() == 0);
    f.seed();
    const auto oldHash = f.library.trackAt(0)->decodedAudioSha256;
    const auto oldCache = read(f.record);
    {
        TagLib::FileRef file(f.audio.toUtf8().constData());
        CHECK(!file.isNull());
        file.tag()->setTitle("MP3 title changed without resetting my grid");
        CHECK(file.save());
    }
    CHECK(f.scan());
    f.checkSetup();
    const auto updated = f.library.trackAt(0);
    CHECK(updated);
    if (updated) {
        CHECK(updated->title == "MP3 title changed without resetting my grid");
        CHECK(updated->decodedAudioSha256 == oldHash);
    }
    f.checkBackup(oldCache);
}

void older_caches_upgrade_without_losing_user_work()
{
    Fixture f;
    f.seed();
    auto old = QJsonDocument::fromJson(read(f.record)).object();
    old.remove("decodedAudioSha256");
    old["futureLocalField"] = "keep me";
    write(f.record, QJsonDocument(old).toJson());
    const auto oldBytes = read(f.record);
    setTime(f.audio, QFileInfo(f.audio).lastModified().addSecs(10));
    CHECK(f.scan()); // exact file SHA proves a timestamp-only change
    f.checkSetup();
    auto upgraded = QJsonDocument::fromJson(read(f.record)).object();
    CHECK(upgraded["decodedAudioSha256"].toString().startsWith("gvpcm1:"));
    CHECK(upgraded["futureLocalField"] == "keep me");
    f.checkBackup(oldBytes);

    // Old caches without a PCM hash cannot prove changed bytes are tag-only.
    // Keep their sole copy intact instead of guessing from approximate hashes.
    upgraded.remove("decodedAudioSha256");
    write(f.record, QJsonDocument(upgraded).toJson());
    const auto legacyBytes = read(f.record);
    {
        TagLib::RIFF::WAV::File file(f.audio.toUtf8().constData());
        file.tag()->setTitle("Unverifiable legacy metadata change");
        CHECK(file.save());
    }
    CHECK(!f.scan());
    CHECK(read(f.record) == legacyBytes);
}

void unreadable_caches_and_failed_backups_are_not_overwritten()
{
    Fixture f;
    f.seed();
    const auto originalCache = read(f.record);
    write(f.record, "unreadable saved setup");
    CHECK(!f.scan());
    CHECK(read(f.record) == "unreadable saved setup");
    write(f.record, originalCache);
    // Make backup creation fail without depending on root/permission semantics.
    CHECK(QDir().rename(f.cache + "/preserved", f.cache + "/backups-kept"));
    write(f.cache + "/preserved", "not a directory");
    setTime(f.audio, QFileInfo(f.audio).lastModified().addSecs(10));
    CHECK(!f.scan());
    CHECK(read(f.record) == originalCache);
    CHECK(f.library.index(0, 5).data(gvt::TrackLibrary::AnalysisErrorRole).toString().contains("back up"));
}

void stale_loaded_decks_cannot_save_against_a_changed_file()
{
    Fixture f;
    f.seed();
    const auto cache = read(f.record);
    auto changed = *f.library.trackAt(0);
    changed.firstBeatSec = 0.123;
    changed.hotCues[0] = 0.456;
    setTime(f.audio, QFileInfo(f.audio).lastModified().addSecs(10));
    QString error;
    CHECK(!f.library.persistBeatGrid(changed, &error));
    CHECK(error.contains("Reload"));
    CHECK(!f.library.persistPerformanceMetadata(changed, &error));
    CHECK(read(f.record) == cache);
    CHECK(f.scan());
    f.checkSetup();
}

void analysis_upgrades_cannot_move_existing_transition_coordinates()
{
    Fixture f;
    f.seed();
    auto previous = QJsonDocument::fromJson(read(f.record)).object();
    previous["analysisVersion"] = 1;
    previous["beatGridSource"] = "analysis";
    write(f.record, QJsonDocument(previous).toJson());
    CHECK(f.scan());
    auto track = f.library.trackAt(0);
    CHECK(track);
    if (!track) return;
    CHECK(track->bpm == previous["bpm"].toDouble());
    CHECK(track->firstBeatSec == previous["firstBeatSec"].toDouble());
    CHECK(track->hotCues[0] == 17.123);
    CHECK(track->savedLoops[2].isSet());
    CHECK(track->analyzedBpm == 0.0); // silent fixture's new detector result
}

void identity_ignores_only_terminal_digital_padding_and_never_shifts_the_start()
{
    const std::vector<float> original {0.0f, 0.0f, .25f, -.25f, .1f, -.1f};
    auto padded = original;
    padded.insert(padded.end(), 176, std::numeric_limits<float>::denorm_min());
    padded.insert(padded.end(), 64, 0.0f);
    CHECK(gvt::detail::decodedAudioHash(original) == gvt::detail::decodedAudioHash(padded));
    auto leading = original;
    leading.insert(leading.begin(), 2, 0.0f);
    CHECK(gvt::detail::decodedAudioHash(original) != gvt::detail::decodedAudioHash(leading));
    auto changedTail = original;
    changedTail.insert(changedTail.end(), 2, 1e-30f); // even extremely quiet normal samples count
    CHECK(gvt::detail::decodedAudioHash(original) != gvt::detail::decodedAudioHash(changedTail));
    auto interior = original;
    interior.insert(interior.begin() + 4, 2, 0.0f);
    CHECK(gvt::detail::decodedAudioHash(original) != gvt::detail::decodedAudioHash(interior));
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // Fixture TrackLibrary construction happens before its constructor body;
    // isolate that initial catalog read as well as all subsequent writes.
    QTemporaryDir initial;
    qputenv("GRAVITINO_CATALOG_PATH", initial.filePath("catalog.json").toUtf8());
    timestamp_changes_keep_the_entire_saved_setup();
    tag_edits_refresh_metadata_without_regridding();
    mp3_tag_edits_keep_the_entire_saved_setup();
    changed_audio_is_refused_even_when_the_timestamp_and_short_fingerprint_match();
    older_caches_upgrade_without_losing_user_work();
    unreadable_caches_and_failed_backups_are_not_overwritten();
    stale_loaded_decks_cannot_save_against_a_changed_file();
    analysis_upgrades_cannot_move_existing_transition_coordinates();
    identity_ignores_only_terminal_digital_padding_and_never_shifts_the_start();
    std::printf("test_cache_identity: %d failures\n", failures);
    return failures ? 1 : 0;
}

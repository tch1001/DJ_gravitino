#include "library/TrackLibrary.h"

#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)
}

int main()
{
    using namespace gvt;
    QTemporaryDir dir;
    CHECK(dir.isValid());
    qputenv("GRAVITINO_TRANSITIONS_DIR", dir.path().toUtf8());

    TransitionStore store;
    GvtFile file;
    file.name = QStringLiteral("Original name");
    file.from.title = QStringLiteral("From");
    file.from.bpm = 128.0;
    file.from.fingerprint = QStringLiteral("gvfp1:from");
    file.from.durationSec = 247.18;
    file.to.title = QStringLiteral("To");
    file.to.bpm = 128.0;
    file.to.fingerprint = QStringLiteral("gvfp1:to");
    file.to.durationSec = 233.89;
    file.masterBpm = 128.0;
    file.events.push_back(
        {0.0, Role::Mixer, ControlId::Crossfader, 0.0, Curve::Step});

    QString error;
    const QString originalPath = store.save(file, &error);
    CHECK(!originalPath.isEmpty());
    CHECK(originalPath.endsWith(QStringLiteral(".transition")));
    CHECK(QFileInfo::exists(originalPath));
    CHECK(store.all().size() == 1);

    file.cues.push_back({0.0, QStringLiteral("Start beatmatch")});
    CHECK(store.update(file, &error));
    CHECK(store.all().size() == 1);
    if (store.all().size() == 1) {
        CHECK(store.all()[0].cues.size() == 1);
        CHECK(store.all()[0].cues[0].label == QStringLiteral("Start beatmatch"));
    }

    TrackData exactFrom;
    exactFrom.title = QStringLiteral("Renamed tags are allowed");
    exactFrom.fingerprint = file.from.fingerprint;
    exactFrom.durationSec = file.from.durationSec;
    TrackData exactTo;
    exactTo.title = QStringLiteral("Another renamed track");
    exactTo.fingerprint = file.to.fingerprint;
    exactTo.durationSec = file.to.durationSec;
    CHECK(store.matching(exactFrom, exactTo).size() == 1);

    // Similar duration is useful as a weak search hint, but must never make
    // an unrelated song operationally inherit another track's transitions.
    TrackData unrelatedFrom;
    unrelatedFrom.title = QStringLiteral("Completely different song");
    unrelatedFrom.fingerprint = QStringLiteral("gvfp1:other");
    unrelatedFrom.durationSec = file.from.durationSec - 0.70;
    CHECK(matchTrack(file.from, unrelatedFrom) == MatchQuality::DurationOnly);
    CHECK(store.matching(unrelatedFrom, exactTo).empty());

    GvtTrackRef identifiedRef;
    identifiedRef.isrc = QStringLiteral("US-ABC-26-00001");
    identifiedRef.bpm = 128.0;
    identifiedRef.durationSec = 180.0;
    identifiedRef.durationBeats = 384.0;
    TrackData identifiedTrack;
    identifiedTrack.isrc = identifiedRef.isrc.toLower();
    identifiedTrack.bpm = 128.1;
    identifiedTrack.durationSec = 180.4;
    identifiedTrack.audibleDurationSec = 180.0;
    CHECK(matchTrack(identifiedRef, identifiedTrack) == MatchQuality::Identifier);
    identifiedTrack.durationSec = 220.0;
    identifiedTrack.audibleDurationSec = 220.0;
    CHECK(matchTrack(identifiedRef, identifiedTrack) != MatchQuality::Identifier);

    const QString renamedPath =
        store.renameTransition(store.all()[0], QStringLiteral("Renamed"), &error);
    CHECK(!renamedPath.isEmpty());
    CHECK(renamedPath != originalPath);
    CHECK(!QFileInfo::exists(originalPath));
    CHECK(QFileInfo::exists(renamedPath));
    CHECK(store.all().size() == 1);
    if (store.all().size() == 1)
        CHECK(store.all()[0].name == QStringLiteral("Renamed"));

    if (!store.all().empty())
        CHECK(store.deleteTransition(store.all()[0], &error));
    CHECK(!QFileInfo::exists(renamedPath));
    CHECK(store.all().empty());

    // A legacy file remains where it is when edited. The portable copy has
    // the same stable identity and therefore supersedes it in the UI.
    GvtFile legacy;
    legacy.name = QStringLiteral("Legacy source");
    legacy.from.title = QStringLiteral("Old From");
    legacy.from.bpm = 120.0;
    legacy.from.durationSec = 60.0;
    legacy.to.title = QStringLiteral("Old To");
    legacy.to.bpm = 120.0;
    legacy.to.durationSec = 60.0;
    legacy.masterBpm = 120.0;
    legacy.events.push_back(
        {0.0, Role::Mixer, ControlId::Crossfader, 0.5, Curve::Step});
    const QString legacyPath = dir.filePath(QStringLiteral("legacy.gvt"));
    CHECK(gvtSaveFile(legacy, legacyPath, &error));
    store.reload();
    CHECK(store.all().size() == 1);
    QString legacyId;
    if (!store.all().empty()) {
        CHECK(store.all()[0].sourceFormat == TransitionSourceFormat::LegacyGvt);
        GvtFile edited = store.all()[0];
        legacyId = edited.id;
        edited.description = QStringLiteral("Converted without overwrite");
        const bool converted = store.update(edited, &error);
        if (!converted)
            std::fprintf(stderr, "legacy conversion error: %s\n",
                         qUtf8Printable(error));
        CHECK(converted);
    }
    CHECK(QFileInfo::exists(legacyPath));
    CHECK(store.all().size() == 1);
    if (!store.all().empty()) {
        CHECK(store.all()[0].sourceFormat == TransitionSourceFormat::PortableYaml);
        CHECK(store.all()[0].id != legacyId);
        CHECK(store.all()[0].legacySourceId == legacyId);
        CHECK(store.all()[0].description ==
              QStringLiteral("Converted without overwrite"));
        CHECK(store.all()[0].filePath != legacyPath);
        CHECK(QFileInfo::exists(store.all()[0].filePath));
    }

    qunsetenv("GRAVITINO_TRANSITIONS_DIR");
    if (failures) return 1;
    std::printf("test_transition_store: save/update/rename/delete passed\n");
    return 0;
}

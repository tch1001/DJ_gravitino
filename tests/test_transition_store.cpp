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

    // Bulk conversion leaves the legacy source in place and exposes both
    // files so the library's format filters can compare them directly.
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
        legacyId = store.all()[0].id;
    }
    QStringList convertedPaths;
    QStringList conversionErrors;
    CHECK(store.convertAllLegacy(&convertedPaths, &conversionErrors) == 1);
    CHECK(conversionErrors.isEmpty());
    CHECK(convertedPaths.size() == 1);
    CHECK(QFileInfo::exists(legacyPath));
    CHECK(store.all().size() == 2);
    const GvtFile* portable = nullptr;
    const GvtFile* legacySource = nullptr;
    for (const GvtFile& candidate : store.all()) {
        if (candidate.sourceFormat == TransitionSourceFormat::PortableYaml)
            portable = &candidate;
        else if (candidate.sourceFormat == TransitionSourceFormat::LegacyGvt)
            legacySource = &candidate;
    }
    CHECK(portable != nullptr);
    CHECK(legacySource != nullptr);
    CHECK(portable && portable->id != legacyId);
    CHECK(portable && portable->legacySourceId == legacyId);
    CHECK(portable && portable->filePath != legacyPath);
    CHECK(portable && QFileInfo::exists(portable->filePath));
    CHECK(legacySource && legacySource->filePath == legacyPath);

    // Re-running conversion is idempotent and cannot accumulate copies.
    convertedPaths.clear();
    CHECK(store.convertAllLegacy(&convertedPaths, &conversionErrors) == 0);
    CHECK(convertedPaths.isEmpty());
    CHECK(conversionErrors.isEmpty());
    CHECK(store.all().size() == 2);

    // Existing portable copies with one recoverable saved-loop slot can be
    // upgraded in place without touching their legacy source.
    GvtFile rawLoop;
    rawLoop.name = QStringLiteral("Raw loop source");
    rawLoop.requirements = {QStringLiteral("timeline.v1"),
                            QStringLiteral("temporary-cues.v1")};
    rawLoop.from.title = QStringLiteral("A");
    rawLoop.from.bpm = 120.0;
    rawLoop.from.durationSec = 60.0;
    rawLoop.to.title = QStringLiteral("B");
    rawLoop.to.bpm = 120.0;
    rawLoop.to.durationSec = 60.0;
    rawLoop.masterBpm = 120.0;
    rawLoop.initialComplete = true;
    rawLoop.initialTo.captured = true;
    rawLoop.initialTo.loopStartBeat = 8.5;
    rawLoop.initialTo.loopEndBeat = 16.5;
    rawLoop.events.push_back(
        {1.0, Role::ToDeck, ControlId::SavedLoop3, 1.0, Curve::Step});
    const QString rawLoopPath = store.save(rawLoop, &error);
    CHECK(!rawLoopPath.isEmpty());
    QStringList upgradedPaths;
    QStringList upgradeErrors;
    CHECK(store.upgradePortableSavedLoops(&upgradedPaths, &upgradeErrors) == 1);
    CHECK(upgradeErrors.isEmpty());
    CHECK(upgradedPaths == QStringList {rawLoopPath});
    GvtFile upgradedLoop;
    CHECK(transitionLoadFile(rawLoopPath, upgradedLoop, &error));
    CHECK(upgradedLoop.transitionLoops.size() == 1);
    CHECK(upgradedLoop.events.size() == 1);
    CHECK(!upgradedLoop.events[0].loopId.isEmpty());
    CHECK(upgradedLoop.requirements.contains(
        QStringLiteral("temporary-loops.v1")));
    CHECK(QFileInfo::exists(legacyPath));

    qunsetenv("GRAVITINO_TRANSITIONS_DIR");
    if (failures) return 1;
    std::printf("test_transition_store: save/update/rename/delete passed\n");
    return 0;
}

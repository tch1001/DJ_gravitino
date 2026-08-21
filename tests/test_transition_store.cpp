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
    file.from.fingerprint = QStringLiteral("gvfp1:from");
    file.from.durationSec = 247.18;
    file.to.title = QStringLiteral("To");
    file.to.fingerprint = QStringLiteral("gvfp1:to");
    file.to.durationSec = 233.89;
    file.masterBpm = 128.0;
    file.events.push_back(
        {0.0, Role::Mixer, ControlId::Crossfader, 0.0, Curve::Step});

    QString error;
    const QString originalPath = store.save(file, &error);
    CHECK(!originalPath.isEmpty());
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

    qunsetenv("GRAVITINO_TRANSITIONS_DIR");
    if (failures) return 1;
    std::printf("test_transition_store: save/update/rename/delete passed\n");
    return 0;
}

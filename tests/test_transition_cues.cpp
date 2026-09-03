// Temporary transition cue tests — semantic cues use an isolated bank and
// preserve both permanent track metadata and hold-CUE/press-PLAY behavior.

#include "audio/AudioEngine.h"
#include "control/ControlBus.h"
#include "transitions/Transition.h"
#include "transitions/TransitionEngine.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cmath>
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace gvt;
    ControlBus bus;
    AudioEngine engine(&bus);
    auto track = std::make_shared<TrackData>();
    track->bpm = 120.0;
    track->firstBeatSec = 0.0;
    track->durationSec = 10.0;
    track->pcm.resize(static_cast<std::size_t>(10 * kSampleRate * 2), 0.1f);
    track->hotCues[0] = 4.0;
    engine.deck(0).loadTrack(track);

    std::array<double, 8> cues {
        1.125, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    engine.deck(0).setTransitionCues(cues);
    bus.dispatch({0, ControlId::TransitionCue1, 1.0}, Origin::Ui);
    CHECK(std::fabs(engine.deck(0).positionSec() - 1.125) < 1e-6);
    CHECK(engine.deck(0).playing.load());
    CHECK(engine.deck(0).previewActive());
    CHECK(track->hotCues[0] == 4.0);

    bus.dispatch({0, ControlId::Play, 1.0}, Origin::Ui);
    bus.dispatch({0, ControlId::TransitionCue1, 0.0}, Origin::Ui);
    CHECK(engine.deck(0).playing.load());
    CHECK(!engine.deck(0).previewActive());
    CHECK(track->hotCues[0] == 4.0);

    engine.deck(0).clearTransitionCues();
    engine.deck(0).stop();
    engine.deck(0).seekSec(3.0);
    bus.dispatch({0, ControlId::TransitionCue1, 1.0}, Origin::Ui);
    CHECK(std::fabs(engine.deck(0).positionSec() - 3.0) < 1e-6);
    CHECK(!engine.deck(0).playing.load());

    // A transition-owned saved loop uses the same isolated CUSTOM bank but
    // retains both IN and OUT. It must not read or overwrite the track's
    // permanent slot at the same pad number.
    track->savedLoops[0].startSec = 6.0;
    track->savedLoops[0].endSec = 7.0;
    std::array<double, 8> loopStarts {
        2.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    std::array<double, 8> loopEnds {
        3.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    engine.deck(0).setTransitionPerformanceSlots(loopStarts, loopEnds);
    engine.deck(0).stop();
    engine.deck(0).seekSec(5.0);
    bus.dispatch({0, ControlId::TransitionCue1, 1.0}, Origin::Ui);
    CHECK(std::fabs(engine.deck(0).positionSec() - 2.0) < 1e-6);
    CHECK(engine.deck(0).loopActive.load());
    CHECK(std::fabs(engine.deck(0).loopStartSec.load() - 2.0) < 1e-6);
    CHECK(std::fabs(engine.deck(0).loopEndSec.load() - 3.0) < 1e-6);
    bus.dispatch({0, ControlId::Play, 1.0}, Origin::Ui);
    bus.dispatch({0, ControlId::TransitionCue1, 0.0}, Origin::Ui);
    CHECK(engine.deck(0).playing.load());
    CHECK(engine.deck(0).loopActive.load());
    CHECK(std::fabs(track->savedLoops[0].startSec - 6.0) < 1e-6);
    CHECK(std::fabs(track->savedLoops[0].endSec - 7.0) < 1e-6);

    GvtFile file;
    file.id = QStringLiteral("cue-file");
    file.sourceFormat = TransitionSourceFormat::PortableYaml;
    file.requirements = {QStringLiteral("timeline.v1"),
                         QStringLiteral("temporary-cues.v1")};
    file.masterBpm = 120.0;
    file.transitionCues.push_back(
        {QStringLiteral("launch"), Role::ToDeck, 2.25,
         QStringLiteral("Launch"), QStringLiteral("start-track"), {}, {},
         QStringLiteral("custom"), 0, {}, {}, {}});
    GvtEvent event;
    event.beat = 0.0;
    event.role = Role::ToDeck;
    event.control = ControlId::HotCue1;
    event.cueId = QStringLiteral("launch");
    file.events.push_back(event);
    track->canonicalBeatOffset = 0.25;
    engine.deck(1).loadTrack(track);
    TransitionPlayer player(&bus, &engine);
    QString error;
    CHECK(player.arm(file, 0, true, &error));
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
    // Canonical beat 2.25 maps to local beat 2.0 for this asset.
    CHECK(std::fabs(engine.deck(1).positionSec() - 1.0) < 1e-6);
    player.abort();

    GvtFile loopFile;
    loopFile.id = QStringLiteral("loop-file");
    loopFile.sourceFormat = TransitionSourceFormat::PortableYaml;
    loopFile.requirements = {QStringLiteral("timeline.v1"),
                             QStringLiteral("temporary-loops.v1")};
    loopFile.masterBpm = 120.0;
    TransitionSavedLoop semanticLoop;
    semanticLoop.id = QStringLiteral("incoming-loop");
    semanticLoop.role = Role::ToDeck;
    semanticLoop.startTrackBeat = 3.25;
    semanticLoop.endTrackBeat = 5.25;
    semanticLoop.preferredPad = 0;
    loopFile.transitionLoops.push_back(semanticLoop);
    GvtEvent loopPress;
    loopPress.role = Role::ToDeck;
    loopPress.control = ControlId::SavedLoop1;
    loopPress.value = 1.0;
    loopPress.loopId = semanticLoop.id;
    GvtEvent play = loopPress;
    play.control = ControlId::Play;
    play.loopId.clear();
    GvtEvent loopRelease = loopPress;
    loopRelease.value = 0.0;
    loopFile.events = {loopPress, play, loopRelease};
    track->savedLoops[0] = SavedLoopSlot {};
    engine.deck(0).stop();
    engine.deck(0).seekSec(0.0);
    CHECK(player.arm(loopFile, 0, true, &error));
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(engine.deck(1).playing.load());
    CHECK(engine.deck(1).loopActive.load());
    // Canonical beats 3.25..5.25 map to local beats 3..5 (1.5..2.5 s).
    CHECK(std::fabs(engine.deck(1).loopStartSec.load() - 1.5) < 1e-6);
    CHECK(std::fabs(engine.deck(1).loopEndSec.load() - 2.5) < 1e-6);
    CHECK(!track->savedLoops[0].isSet());
    player.abort();

    GvtFile unsupported = file;
    unsupported.unsupportedRequirements = {QStringLiteral("warp-grid.v9")};
    CHECK(!player.arm(unsupported, 0, true, &error));
    CHECK(error.contains(QStringLiteral("warp-grid.v9")));

    if (failures) return 1;
    std::printf("test_transition_cues: isolated cue bank passed\n");
    return 0;
}

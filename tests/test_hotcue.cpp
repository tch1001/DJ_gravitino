#include "audio/AudioEngine.h"
#include "control/ControlBus.h"
#include "transitions/TransitionEngine.h"

#include <cmath>
#include <cstdio>
#include <memory>

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
    ControlBus bus;
    AudioEngine engine(&bus);

    auto track = std::make_shared<TrackData>();
    track->durationSec = 4.0;
    track->bpm = 120.0;
    track->firstBeatSec = 0.0;
    track->pcm.resize(static_cast<std::size_t>(kSampleRate) * 4U * 2U);
    engine.deck(0).loadTrack(track);

    // First press stores an unset cue without starting playback.
    engine.deck(0).seekSec(1.0);
    bus.dispatch({0, ControlId::HotCue1, 1.0}, Origin::Ui);
    CHECK(!engine.deck(0).playing.load());
    CHECK(std::fabs(track->hotCues[0] - 1.0) < 1e-6);

    // A set hot cue jumps there and previews immediately while held.
    engine.deck(0).seekSec(2.0);
    engine.deck(0).play();
    bus.dispatch({0, ControlId::HotCue1, 1.0}, Origin::Ui);
    CHECK(engine.deck(0).playing.load());
    CHECK(std::fabs(engine.deck(0).positionSec() - 1.0) < 1e-6);

    // Let the preview advance, then releasing pauses and snaps back.
    float scratch[480 * 2] {};
    engine.renderOffline(scratch, 480);
    CHECK(engine.deck(0).positionSec() > 1.0);
    bus.dispatch({0, ControlId::HotCue1, 0.0}, Origin::Ui);
    CHECK(!engine.deck(0).playing.load());
    CHECK(std::fabs(engine.deck(0).positionSec() - 1.0) < 1e-6);

    // PLAY while a hot cue is held takes over the preview. Releasing the pad
    // must keep transport rolling instead of stopping and snapping back.
    engine.deck(0).seekSec(2.0);
    bus.dispatch({0, ControlId::HotCue1, 1.0}, Origin::Ui);
    engine.renderOffline(scratch, 480);
    bus.dispatch({0, ControlId::Play, 1.0}, Origin::Ui);
    CHECK(!engine.deck(0).previewActive());
    const double hotCueLatchedSec = engine.deck(0).positionSec();
    bus.dispatch({0, ControlId::HotCue1, 0.0}, Origin::Ui);
    CHECK(engine.deck(0).playing.load());
    CHECK(engine.deck(0).positionSec() >= hotCueLatchedSec);

    // The same takeover applies to the ordinary CUE button.
    engine.deck(0).stop();
    engine.deck(0).cuePointSec.store(0.5);
    engine.deck(0).seekSec(0.5);
    bus.dispatch({0, ControlId::Cue, 1.0}, Origin::Ui);
    CHECK(engine.deck(0).previewActive());
    engine.renderOffline(scratch, 480);
    bus.dispatch({0, ControlId::Play, 1.0}, Origin::Ui);
    CHECK(!engine.deck(0).previewActive());
    const double cueLatchedSec = engine.deck(0).positionSec();
    bus.dispatch({0, ControlId::Cue, 0.0}, Origin::Ui);
    CHECK(engine.deck(0).playing.load());
    CHECK(engine.deck(0).positionSec() >= cueLatchedSec);
    engine.deck(0).stop();

    // Quantize defaults on per deck. Placement snaps to the nearest whole
    // beat, and triggering also snaps legacy/off-grid saved cues.
    CHECK(engine.deck(0).quantizeHotCues.load());
    track->firstBeatSec = 0.1;
    engine.deck(0).seekSec(0.38); // beat 0.56 -> beat 1 at 0.6 s
    engine.deck(0).setHotCue(1);
    CHECK(std::fabs(track->hotCues[1] - 0.6) < 1e-6);

    track->hotCues[2] = 0.82; // beat 1.44 -> beat 1 at 0.6 s
    engine.deck(0).seekSec(2.0);
    engine.deck(0).handleHotCue(2, true);
    CHECK(engine.deck(0).previewActive());
    CHECK(std::fabs(engine.deck(0).positionSec() - 0.6) < 1e-6);
    engine.deck(0).handleHotCue(2, false);
    CHECK(!engine.deck(0).playing.load());
    CHECK(std::fabs(engine.deck(0).positionSec() - 0.6) < 1e-6);

    engine.deck(0).seekSec(1.31); // direct jump path also quantizes off-grid cue
    engine.deck(0).jumpHotCue(2);
    CHECK(std::fabs(engine.deck(0).positionSec() - 0.6) < 1e-6);

    // With Quantize off, both placement and triggering preserve exact time.
    bus.dispatch({0, ControlId::Quantize, 0.0}, Origin::Ui);
    CHECK(!engine.deck(0).quantizeHotCues.load());
    engine.deck(0).seekSec(1.37);
    engine.deck(0).setHotCue(3);
    CHECK(std::fabs(track->hotCues[3] - 1.37) < 1e-6);
    engine.deck(0).seekSec(2.0);
    engine.deck(0).handleHotCue(3, true);
    CHECK(std::fabs(engine.deck(0).positionSec() - 1.37) < 1e-6);
    engine.deck(0).play(); // retain the existing PLAY-latch contract
    engine.deck(0).handleHotCue(3, false);
    CHECK(engine.deck(0).playing.load());
    CHECK(std::fabs(engine.deck(0).positionSec() - 1.37) < 1e-6);
    engine.deck(0).stop();

    // Manual loop IN/OUT follows the same per-deck Quantize preference.
    // Enabled means whole beat lines; disabled preserves exact pointer time.
    engine.deck(0).quantizeHotCues.store(true);
    track->bpm = 120.0;
    track->firstBeatSec = 0.1;
    engine.deck(0).seekSec(0.38); // nearest whole beat is 0.6 s
    engine.deck(0).loopIn();
    engine.deck(0).seekSec(1.38); // nearest whole beat is 1.6 s
    engine.deck(0).loopOut();
    CHECK(engine.deck(0).loopActive.load());
    CHECK(std::fabs(engine.deck(0).loopStartSec.load() - 0.6) < 1e-6);
    CHECK(std::fabs(engine.deck(0).loopEndSec.load() - 1.6) < 1e-6);
    engine.deck(0).loopExit();

    engine.deck(0).quantizeHotCues.store(false);
    engine.deck(0).seekSec(0.73);
    engine.deck(0).loopIn();
    engine.deck(0).seekSec(1.41);
    engine.deck(0).loopOut();
    CHECK(engine.deck(0).loopActive.load());
    CHECK(std::fabs(engine.deck(0).loopStartSec.load() - 0.73) < 1e-6);
    CHECK(std::fabs(engine.deck(0).loopEndSec.load() - 1.41) < 1e-6);
    engine.deck(0).loopExit();

    // A missing beat grid degrades safely to exact positioning even while the
    // user's Quantize preference remains enabled.
    engine.deck(0).quantizeHotCues.store(true);
    track->bpm = 0.0;
    engine.deck(0).seekSec(1.23);
    engine.deck(0).setHotCue(4);
    CHECK(std::fabs(track->hotCues[4] - 1.23) < 1e-6);
    track->bpm = 120.0;
    track->firstBeatSec = 0.0;

    // A transition that references a hot cue stores the pad's track-relative
    // beat so Tutorial can later reject missing or incorrectly mapped pads.
    auto incoming = std::make_shared<TrackData>();
    incoming->durationSec = 4.0;
    incoming->bpm = 120.0;
    incoming->pcm.resize(static_cast<std::size_t>(kSampleRate) * 4U * 2U);
    engine.deck(1).loadTrack(incoming);
    TransitionRecorder recorder(&bus, &engine);
    recorder.start(0);
    bus.dispatch({0, ControlId::HotCue1, 1.0}, Origin::Ui);
    bus.dispatch({0, ControlId::HotCue1, 0.0}, Origin::Ui);
    const GvtFile recorded = recorder.finish();
    CHECK(std::fabs(recorded.fromHotCueBeats[0] - 2.0) < 1e-6);

    // Audio before the detected first downbeat has a negative but fully valid
    // grid position. Recording must retain it instead of treating it as an
    // unset hot-cue mapping.
    track->firstBeatSec = 2.0;
    track->hotCues[0] = 0.5;
    recorder.start(0);
    bus.dispatch({0, ControlId::HotCue1, 1.0}, Origin::Ui);
    bus.dispatch({0, ControlId::HotCue1, 0.0}, Origin::Ui);
    const GvtFile negativeCueRecorded = recorder.finish();
    CHECK(std::fabs(negativeCueRecorded.fromHotCueBeats[0] + 3.0) < 1e-6);
    track->firstBeatSec = 0.0;

    // A combined CUSTOM/saved-loop pad has the same hold/PLAY-latch contract
    // as a hot cue, and its release-aware event is transition-recordable.
    incoming->savedLoops[0].startSec = 0.5;
    incoming->savedLoops[0].endSec = 1.5;
    incoming->savedLoops[0].label = QStringLiteral("Intro loop");
    engine.deck(1).stop();
    engine.deck(1).loopExit();
    recorder.start(0);
    bus.dispatch({1, ControlId::SavedLoop1, 1.0}, Origin::Ui);
    CHECK(engine.deck(1).previewActive());
    CHECK(engine.deck(1).playing.load());
    CHECK(std::fabs(engine.deck(1).positionSec() - 0.5) < 1e-6);
    engine.renderOffline(scratch, 480);
    bus.dispatch({1, ControlId::SavedLoop1, 0.0}, Origin::Ui);
    CHECK(!engine.deck(1).previewActive());
    CHECK(!engine.deck(1).playing.load());
    CHECK(std::fabs(engine.deck(1).positionSec() - 0.5) < 1e-6);
    const GvtFile loopRecorded = recorder.finish();
    bool foundLoopPress = false;
    bool foundLoopRelease = false;
    for (const GvtEvent& event : loopRecorded.events) {
        if (event.role == Role::ToDeck &&
            event.control == ControlId::SavedLoop1)
            event.value >= 0.5 ? foundLoopPress = true
                               : foundLoopRelease = true;
    }
    CHECK(foundLoopPress && foundLoopRelease);
    CHECK(std::fabs(loopRecorded.anchorToBeat - 1.0) < 1e-6);

    bus.dispatch({1, ControlId::SavedLoop1, 1.0}, Origin::Ui);
    engine.renderOffline(scratch, 480);
    bus.dispatch({1, ControlId::Play, 1.0}, Origin::Ui);
    CHECK(!engine.deck(1).previewActive());
    const double savedLoopLatchedSec = engine.deck(1).positionSec();
    bus.dispatch({1, ControlId::SavedLoop1, 0.0}, Origin::Ui);
    CHECK(engine.deck(1).playing.load());
    CHECK(engine.deck(1).positionSec() >= savedLoopLatchedSec);

    if (failures) return 1;
    std::printf("test_hotcue: quantize, hold-preview, and release-return passed\n");
    return 0;
}

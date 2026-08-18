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

    if (failures) return 1;
    std::printf("test_hotcue: hold-preview and release-return passed\n");
    return 0;
}

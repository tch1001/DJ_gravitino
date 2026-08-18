#include "audio/AudioEngine.h"
#include "control/ControlBus.h"

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

bool near(double actual, double expected, double tolerance = 1.0e-6)
{
    return std::fabs(actual - expected) <= tolerance;
}

gvt::TrackDataPtr makeTrack(double bpm = 120.0)
{
    auto track = std::make_shared<gvt::TrackData>();
    track->durationSec = 4.0;
    track->bpm = bpm;
    track->firstBeatSec = 0.0;
    track->pcm.assign(
        static_cast<std::size_t>(gvt::kSampleRate) * 4U * 2U, 0.2f);
    return track;
}
} // namespace

int main()
{
    using namespace gvt;

    ControlBus bus;
    AudioEngine engine(&bus);
    Deck& deck = engine.deck(0);
    deck.loadTrack(makeTrack());

    // SYNC is a one-shot phase alignment. It must not touch tempo/effective
    // BPM, even when the other deck is running at a different tempo.
    Deck& other = engine.deck(1);
    other.loadTrack(makeTrack(100.0));
    deck.tempoRatio.store(0.93);
    other.tempoRatio.store(1.07);
    deck.seekSec(1.13);
    other.seekSec(0.37);
    const double ratioBeforeSync = deck.tempoRatio.load();
    const double bpmBeforeSync = deck.effectiveBpm();
    bus.dispatch({0, ControlId::TempoSync, 1.0}, Origin::Midi);
    CHECK(near(deck.tempoRatio.load(), ratioBeforeSync));
    CHECK(near(deck.effectiveBpm(), bpmBeforeSync));
    const double targetPhase = deck.beatPosition() -
                               std::floor(deck.beatPosition());
    const double otherPhase = other.beatPosition() -
                              std::floor(other.beatPosition());
    CHECK(near(targetPhase, otherPhase));
    deck.tempoRatio.store(1.0);

    // A live regrid updates both the mutable TrackData used by beat actions
    // and Deck's copied realtime timing state without requiring a reload.
    deck.seekSec(2.0);
    CHECK(near(deck.beatPosition(), 4.0));
    CHECK(near(deck.effectiveBpm(), 120.0));
    deck.updateBeatGrid(100.0, 0.2);
    CHECK(near(deck.beatPosition(), 3.0));
    CHECK(near(deck.effectiveBpm(), 100.0));
    CHECK(near(deck.track()->bpm, 100.0));
    CHECK(near(deck.track()->firstBeatSec, 0.2));
    deck.updateBeatGrid(120.0, 0.0);

    // Platter movement is positional and deliberately does not beat-snap.
    deck.seekSec(1.003);
    bus.dispatch({0, ControlId::PlatterScratch, 2.0}, Origin::Midi);
    CHECK(near(deck.positionSec(), 1.023));
    bus.dispatch({0, ControlId::PlatterScratch, -3.0}, Origin::Midi);
    CHECK(near(deck.positionSec(), 0.993));

    // Track bounds stay on playable PCM rather than crossing before zero or
    // to the frame just past EOF.
    bus.dispatch({0, ControlId::PlatterScratch, -10000.0}, Origin::Midi);
    CHECK(near(deck.positionSec(), 0.0));
    bus.dispatch({0, ControlId::PlatterScratch, 10000.0}, Origin::Midi);
    CHECK(deck.positionSec() < 4.0);
    CHECK(deck.positionSec() > 3.999);

    // An active loop is preserved and acts as the platter's clamp range.
    deck.seekSec(1.2);
    deck.loopAuto(2.0); // 1.0s to 2.0s at 120 BPM
    CHECK(deck.loopActive.load());
    CHECK(near(deck.loopStartSec.load(), 1.0));
    CHECK(near(deck.loopEndSec.load(), 2.0));
    bus.dispatch({0, ControlId::PlatterScratch, -1000.0}, Origin::Midi);
    CHECK(near(deck.positionSec(), 1.0));
    CHECK(deck.loopActive.load());
    bus.dispatch({0, ControlId::PlatterScratch, 1000.0}, Origin::Midi);
    CHECK(deck.positionSec() < 2.0);
    CHECK(deck.positionSec() > 1.999);
    CHECK(deck.loopActive.load());

    // Saved-loop pad triggering uses exact authored bounds, starts transport,
    // and returns to IN on every press like a one-shot hot cue.
    deck.loopExit();
    deck.seekSec(3.0);
    CHECK(deck.retriggerSavedLoop(0.75, 1.25));
    CHECK(deck.loopActive.load());
    CHECK(deck.playing.load());
    CHECK(near(deck.loopStartSec.load(), 0.75));
    CHECK(near(deck.loopEndSec.load(), 1.25));
    CHECK(near(deck.positionSec(), 0.75));
    deck.seekSec(1.1);
    CHECK(deck.retriggerSavedLoop(0.75, 1.25));
    CHECK(deck.playing.load());
    CHECK(near(deck.positionSec(), 0.75));
    deck.stop();
    deck.loopExit();

    // Touch-gated scratch is silent while stationary and reads the track in
    // either direction only when wheel motion arrives. Normal play resumes
    // on release if and only if it was running before the touch.
    deck.seekSec(2.0);
    deck.play();
    bus.dispatch({0, ControlId::PlatterTouch, 1.0}, Origin::Midi);
    CHECK(!deck.playing.load());
    float audio[256 * 2] {};
    engine.renderOffline(audio, 256);
    double energy = 0.0;
    for (float sample : audio) energy += std::abs(sample);
    CHECK(near(energy, 0.0));
    CHECK(near(deck.positionSec(), 2.0));

    bus.dispatch({0, ControlId::PlatterScratch, 1.0}, Origin::Midi);
    engine.renderOffline(audio, 256);
    energy = 0.0;
    for (float sample : audio) energy += std::abs(sample);
    CHECK(energy > 1.0);
    const double afterForwardScratch = deck.positionSec();
    CHECK(afterForwardScratch > 2.0);

    bus.dispatch({0, ControlId::PlatterScratch, -1.0}, Origin::Midi);
    engine.renderOffline(audio, 256);
    CHECK(deck.positionSec() < afterForwardScratch);
    bus.dispatch({0, ControlId::PlatterTouch, 0.0}, Origin::Midi);
    CHECK(deck.playing.load());
    deck.stop();

    bus.dispatch({0, ControlId::PlatterTouch, 1.0}, Origin::Midi);
    bus.dispatch({0, ControlId::PlatterTouch, 0.0}, Origin::Midi);
    CHECK(!deck.playing.load());

    // Non-finite input is ignored safely.
    const double before = deck.positionSec();
    deck.scratch(std::nan(""));
    CHECK(near(deck.positionSec(), before));

    if (failures)
        return 1;
    std::printf("test_jog_scratch: platter scrubs safely; rim semantics stay separate\n");
    return 0;
}

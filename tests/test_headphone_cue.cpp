#include "audio/AudioEngine.h"
#include "control/ControlBus.h"
#include "midi/Flx4Mapping.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

gvt::TrackDataPtr constantTrack(float sample)
{
    auto track = std::make_shared<gvt::TrackData>();
    track->durationSec = 2.0;
    track->bpm = 120.0;
    track->pcm.assign(
        static_cast<std::size_t>(gvt::kSampleRate) * 2U * 2U, sample);
    return track;
}
}

int main()
{
    using namespace gvt;

    // Official FLX4 mixer messages: channel CUE on 90/91 54, master CUE on
    // 96 63, and the 14-bit headphones CUE/MASTER mix on B6 0C/2C.
    Flx4Mapping mapping;
    auto event = mapping.parse(std::array<unsigned char, 3>{0x90, 0x54, 0x7F});
    CHECK(event && event->deck == 0 &&
          event->id == ControlId::HeadphoneCue);
    event = mapping.parse(std::array<unsigned char, 3>{0x91, 0x54, 0x7F});
    CHECK(event && event->deck == 1 &&
          event->id == ControlId::HeadphoneCue);
    event = mapping.parse(std::array<unsigned char, 3>{0x96, 0x63, 0x7F});
    CHECK(event && event->deck == kNoDeck &&
          event->id == ControlId::MasterCue);
    CHECK(!mapping.parse(
        std::array<unsigned char, 3>{0xB6, 0x0C, 0x40}));
    event = mapping.parse(std::array<unsigned char, 3>{0xB6, 0x2C, 0x00});
    CHECK(event && event->id == ControlId::HeadphoneMix &&
          event->value > 0.49 && event->value < 0.51);
    const auto led = Flx4Mapping::ledMessage(
        1, ControlId::HeadphoneCue, true);
    CHECK(led && (*led)[0] == 0x91 && (*led)[1] == 0x54 &&
          (*led)[2] == 0x7F);

    ControlBus bus;
    AudioEngine engine(&bus);
    const TrackDataPtr a = constantTrack(0.20f);
    const TrackDataPtr b = constantTrack(-0.40f);
    engine.deck(0).loadTrack(a);
    engine.deck(1).loadTrack(b);
    engine.deck(0).play();
    engine.deck(1).play();
    engine.deck(1).fader.store(0.0f);
    engine.crossfader.store(0.0f);

    // Deck B is absent from master (fader down + crossfader on A) but its PFL
    // must be present on FLX4 outputs 3/4 when channel CUE is selected.
    bus.dispatch({1, ControlId::HeadphoneCue, 1.0}, Origin::Midi);
    bus.dispatch({kNoDeck, ControlId::HeadphoneMix, 0.0}, Origin::Midi);
    std::vector<float> fourChannel(256U * 4U);
    engine.renderOfflineFourChannel(fourChannel.data(), 256);
    for (int frame = 0; frame < 256; ++frame) {
        CHECK(fourChannel[(std::size_t)frame * 4U] > 0.10f);      // master A
        CHECK(fourChannel[(std::size_t)frame * 4U + 2U] < -0.20f); // phones B
    }

    // At full MASTER with master CUE enabled, phones mirror the master bus.
    bus.dispatch({1, ControlId::HeadphoneCue, 0.0}, Origin::Midi);
    bus.dispatch({kNoDeck, ControlId::MasterCue, 1.0}, Origin::Midi);
    bus.dispatch({kNoDeck, ControlId::HeadphoneMix, 1.0}, Origin::Midi);
    engine.renderOfflineFourChannel(fourChannel.data(), 256);
    for (int frame = 0; frame < 256; ++frame) {
        const auto base = (std::size_t)frame * 4U;
        CHECK(std::fabs(fourChannel[base] - fourChannel[base + 2U]) < 1e-6f);
    }

    // A hot cue after track replacement must render the replacement PCM,
    // never the prior song's source or cue table.
    const TrackDataPtr old = constantTrack(0.35f);
    const TrackDataPtr replacement = constantTrack(-0.35f);
    old->hotCues[0] = 0.25;
    replacement->hotCues[0] = 0.75;
    engine.deck(1).stop();
    engine.deck(0).loadTrack(old);
    engine.deck(0).loadTrack(replacement);
    bus.dispatch({0, ControlId::HotCue1, 1.0}, Origin::Midi);
    CHECK(engine.deck(0).track() == replacement);
    CHECK(std::fabs(engine.deck(0).positionSec() - 0.75) < 1e-6);
    std::vector<float> stereo(256U * 2U);
    engine.renderOffline(stereo.data(), 256);
    for (float sample : stereo) CHECK(sample < -0.20f);

    if (failures) return 1;
    std::printf("test_headphone_cue: FLX4 PFL routing and hot-cue source passed\n");
    return 0;
}

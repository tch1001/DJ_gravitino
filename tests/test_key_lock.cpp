#include "audio/AudioEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        ++failures; \
    } \
} while (0)

gvt::TrackDataPtr sineTrack(double frequency)
{
    auto track = std::make_shared<gvt::TrackData>();
    track->durationSec = 6.0;
    track->bpm = 120.0;
    track->firstBeatSec = 0.0;
    const int frames = gvt::kSampleRate * 6;
    track->pcm.resize(static_cast<std::size_t>(frames) * 2U);
    for (int frame = 0; frame < frames; ++frame) {
        const float sample = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * frequency * frame /
                           gvt::kSampleRate));
        track->pcm[static_cast<std::size_t>(frame) * 2U] = sample;
        track->pcm[static_cast<std::size_t>(frame) * 2U + 1U] = sample;
    }
    return track;
}

std::vector<float> renderOneSecond(gvt::Deck& deck)
{
    std::vector<float> result(
        static_cast<std::size_t>(gvt::kSampleRate) * 2U);
    constexpr int chunk = 256;
    for (int offset = 0; offset < gvt::kSampleRate; offset += chunk) {
        const int frames = std::min(chunk, gvt::kSampleRate - offset);
        deck.render(result.data() + static_cast<std::size_t>(offset) * 2U,
                    frames);
    }
    return result;
}

double positiveCrossingFrequency(const std::vector<float>& stereo,
                                 double fromSec, double toSec)
{
    const int first = static_cast<int>(fromSec * gvt::kSampleRate);
    const int last = static_cast<int>(toSec * gvt::kSampleRate);
    int crossings = 0;
    for (int frame = first + 1; frame < last; ++frame) {
        const float previous = stereo[
            static_cast<std::size_t>(frame - 1) * 2U];
        const float current = stereo[static_cast<std::size_t>(frame) * 2U];
        if (previous <= 0.0f && current > 0.0f)
            ++crossings;
    }
    return crossings / (toSec - fromSec);
}
}

int main()
{
    using namespace gvt;
    constexpr double sourceHz = 440.0;
    constexpr double ratio = 1.08;

    Deck deck;
    deck.loadTrack(sineTrack(sourceHz));
    deck.tempoRatio.store(ratio);
    deck.preservePitch.store(false);
    deck.play();
    const std::vector<float> vinyl = renderOneSecond(deck);
    const double vinylHz = positiveCrossingFrequency(vinyl, 0.25, 0.85);
    CHECK(vinylHz > 468.0 && vinylHz < 482.0);

    deck.loadTrack(sineTrack(sourceHz));
    deck.tempoRatio.store(ratio);
    deck.preservePitch.store(true);
    deck.play();
    const std::vector<float> locked = renderOneSecond(deck);
    const double lockedHz = positiveCrossingFrequency(locked, 0.25, 0.85);
    CHECK(lockedHz > 432.0 && lockedHz < 448.0);
    CHECK(std::fabs(deck.positionSec() - ratio) < 2.0 / kSampleRate);

    if (failures) return 1;
    std::printf("test_key_lock: %.1f Hz vinyl, %.1f Hz preserved\n",
                vinylHz, lockedHz);
    return 0;
}

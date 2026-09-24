#include "audio/AudioEngine.h"
#include "control/ControlBus.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
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
    ControlBus bus;
    AudioEngine engine(&bus);
    Deck& deck = engine.deck(0);

    const TrackDataPtr oldTrack = constantTrack(0.30f);
    const TrackDataPtr newTrack = constantTrack(-0.30f);
    deck.loadTrack(oldTrack);
    deck.stemVocals.store(0.0f);
    deck.stemMelody.store(0.25f);
    deck.stemBass.store(0.5f);
    deck.stemDrums.store(0.75f);
    engine.deck(1).loadTrack(oldTrack);
    engine.deck(1).stemVocals.store(0.1f);
    engine.deck(1).stemMelody.store(0.2f);
    engine.deck(1).stemBass.store(0.3f);
    engine.deck(1).stemDrums.store(0.4f);
    auto oldStems = std::make_shared<StemSet>();
    for (auto* samples : {&oldStems->vocals, &oldStems->melody,
                         &oldStems->bass, &oldStems->drums})
        samples->assign(oldTrack->pcm.size(), 2000);
    deck.attachStems(oldStems);
    CHECK(deck.stemsAttached());
    deck.play();

    // Keep the real render path active while the GUI thread replaces the
    // source. loadTrack must drain an in-flight render before returning.
    std::atomic<bool> keepRendering {true};
    std::atomic<bool> swapComplete {false};
    std::atomic<bool> heardOldAfterSwap {false};
    std::atomic<int> verifiedCallbacks {0};
    std::thread audio([&] {
        std::vector<float> output(256U * 2U);
        while (keepRendering.load(std::memory_order_acquire)) {
            // Snapshot before rendering: an old callback that finishes just
            // as loadTrack returns must not be misclassified as post-swap.
            const bool verify = swapComplete.load(std::memory_order_acquire);
            engine.renderOffline(output.data(), 256);
            if (verify) {
                for (float sample : output) {
                    if (sample > 1.0e-5f)
                        heardOldAfterSwap.store(true,
                                                std::memory_order_release);
                }
                verifiedCallbacks.fetch_add(1, std::memory_order_release);
            }
        }
    });

    deck.loadTrack(newTrack);
    CHECK(!deck.playing.load());
    CHECK(deck.track() == newTrack);
    CHECK(std::fabs(deck.positionSec()) < 1.0e-9);
    CHECK(!deck.stemsAttached());
    CHECK(deck.stemVocals.load() == 1.0f);
    CHECK(deck.stemMelody.load() == 1.0f);
    CHECK(deck.stemBass.load() == 1.0f);
    CHECK(deck.stemDrums.load() == 1.0f);
    CHECK(engine.deck(1).stemVocals.load() == 0.1f);
    CHECK(engine.deck(1).stemMelody.load() == 0.2f);
    CHECK(engine.deck(1).stemBass.load() == 0.3f);
    CHECK(engine.deck(1).stemDrums.load() == 0.4f);
    deck.play();
    swapComplete.store(true, std::memory_order_release);

    // Render enough callbacks after the swap to catch a retained PCM source
    // or an undrained old callback. The replacement track is entirely
    // negative, so any positive audio must have come from the old track.
    while (verifiedCallbacks.load(std::memory_order_acquire) < 64 &&
           !heardOldAfterSwap.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    keepRendering.store(false, std::memory_order_release);
    audio.join();
    CHECK(!heardOldAfterSwap.load());

    // A deterministic post-swap render should contain the new source only.
    deck.seekSec(0.5);
    deck.play();
    std::vector<float> output(256U * 2U);
    engine.renderOffline(output.data(), 256);
    for (float sample : output) CHECK(sample < -0.20f);

    // Reset only happens on load, not when stems finish preparing or when a
    // transition intentionally mutes one. EQ/faders and the other deck persist.
    bus.dispatch({0, ControlId::StemMelody, 0.0}, Origin::Replay);
    deck.attachStems(oldStems);
    CHECK(deck.stemMelody.load() == 0.0f);
    deck.eqLow.store(0.37f);
    deck.fader.store(0.6f);
    deck.loadTrack(nullptr);
    CHECK(deck.stemVocals.load() == 1.0f && deck.stemMelody.load() == 1.0f);
    CHECK(deck.stemBass.load() == 1.0f && deck.stemDrums.load() == 1.0f);
    CHECK(deck.eqLow.load() == 0.37f && deck.fader.load() == 0.6f);

    if (failures) return 1;
    std::printf("test_deck_swap: old source fully replaced under render load\n");
    return 0;
}

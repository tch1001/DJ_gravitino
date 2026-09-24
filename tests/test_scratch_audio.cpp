// Protect continuous scratch audio across sparse MIDI packets, source-rate
// changes and transport boundaries; no controller or user music is required.
#include "audio/AudioEngine.h"
#include "audio/ScratchDsp.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <vector>

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); ++failures; \
} } while (0)

gvt::TrackDataPtr tone(double frequency = 440.0)
{
    auto track = std::make_shared<gvt::TrackData>();
    track->bpm = 120.0;
    track->durationSec = 4.0;
    track->pcm.resize(4 * gvt::kSampleRate * 2);
    for (int frame = 0; frame < 4 * gvt::kSampleRate; ++frame) {
        const float value = frequency == 0.0 ? 0.2f :
            0.2f * std::sin(2.0 * std::numbers::pi * frequency * frame / gvt::kSampleRate);
        track->pcm[2 * frame] = track->pcm[2 * frame + 1] = value;
    }
    return track;
}

void render(gvt::Deck& deck, int frames, int block = 256)
{
    std::vector<float> audio(2 * block);
    while (frames > 0) {
        const int count = std::min(frames, block);
        deck.render(audio.data(), count);
        for (int i = 0; i < count * 2; ++i) CHECK(std::isfinite(audio[i]));
        frames -= count;
    }
}

void sparse_packets_do_not_make_silent_blocks()
{
    gvt::Deck deck;
    deck.loadTrack(tone());
    deck.seekSec(1.0);
    deck.beginScratch();
    std::array<float, 256 * 2> audio {};
    double minEnergy = 1.0;
    // One MIDI report every 16 ms: the old code had TWO silent blocks after
    // every audible block, despite the hand continually rotating the platter.
    for (int block = 0; block < 90; ++block) {
        if (block % 3 == 0) deck.scratch(1.6);
        deck.render(audio.data(), 256);
        double energy = 0.0;
        for (float sample : audio) energy += sample * sample / audio.size();
        if (block > 6) minEnergy = std::min(minEnergy, energy);
    }
    CHECK(minEnergy > 0.005);
    render(deck, 8000);
    CHECK(std::abs(deck.positionSec() - 1.48) < 1.0e-7);
    deck.render(audio.data(), 256);
    for (float sample : audio) CHECK(std::abs(sample) < 1.0e-6);
    deck.endScratch();
    CHECK(!deck.playing.load());
}

std::vector<float> render_packet_sequence(int blockSize)
{
    gvt::Deck deck;
    deck.loadTrack(tone());
    deck.seekSec(1.0);
    deck.beginScratch();
    std::vector<float> result(24000 * 2);
    for (int frame = 0; frame < 24000;) {
        // Irregular but identical sample-timed input for every callback size.
        if (frame % 720 == 0) deck.scratch(frame < 12000 ? 1.5 : -1.5);
        const int count = std::min({blockSize, 720 - frame % 720, 24000 - frame});
        deck.render(result.data() + 2 * frame, count);
        frame += count;
    }
    return result;
}

void callback_size_does_not_change_the_motion_or_audio()
{
    const auto reference = render_packet_sequence(256);
    for (int block : {32, 64, 128, 512, 1024}) {
        const auto actual = render_packet_sequence(block);
        double error = 0.0;
        for (std::size_t i = 0; i < reference.size(); ++i)
            error = std::max(error, static_cast<double>(std::abs(actual[i] - reference[i])));
        CHECK(error < 1.0e-6);
    }
}

void fast_scratches_are_band_limited_in_both_directions()
{
    gvt::ScratchResampler resampler;
    for (double speed : {4.0, -4.0, 12.0, -12.0}) {
        for (double frequency : {500.0, 14000.0}) {
            double energy = 0.0;
            for (int i = 0; i < 4096; ++i) {
                const auto sample = resampler.sample(100000.37 + speed * i, speed,
                    [&](double frame) {
                        const float value = std::sin(2 * std::numbers::pi * frequency * frame /
                                                     gvt::kSampleRate);
                        return std::array<float, 2> {value, value};
                    });
                energy += sample[0] * sample[0] / 4096;
                CHECK(sample[0] == sample[1]);
            }
            if (frequency == 500.0) CHECK(energy > 0.4);
            else CHECK(energy < 0.0001); // >37 dB rejection of an aliased tone
        }
    }
}

void releases_keep_the_final_hand_position_and_transport_state()
{
    for (bool playing : {false, true}) {
        for (bool keyLock : {false, true}) {
            gvt::Deck deck;
            deck.loadTrack(tone());
            deck.preservePitch.store(keyLock);
            deck.seekSec(1.0);
            if (playing) deck.play();
            deck.beginScratch();
            deck.scratch(2.0);
            render(deck, 256);
            // Include a packet just before release, with no intervening render.
            deck.scratch(-0.5);
            deck.endScratch();
            CHECK(std::abs(deck.positionSec() - 1.015) < 1.0e-9);
            CHECK(deck.playing.load() == playing);
            render(deck, 256);
            const double expected = 1.015 + (playing ? 256.0 / gvt::kSampleRate : 0.0);
            CHECK(std::abs(deck.positionSec() - expected) < 1.0e-9);
        }
    }
}

void touches_and_releases_do_not_click_or_leave_a_dc_tail()
{
    gvt::Deck deck;
    deck.loadTrack(tone(0.0));
    deck.seekSec(1.0);
    deck.play();
    std::array<float, 512> audio {};
    deck.render(audio.data(), 256);
    float previous = audio.back();
    deck.beginScratch();
    deck.render(audio.data(), 256);
    CHECK(std::abs(audio[0] - previous) < 1.0e-6);
    for (int i = 2; i < 512; i += 2) CHECK(std::abs(audio[i] - audio[i - 2]) < 0.01);
    CHECK(std::abs(audio.back()) < 1.0e-6);
    deck.endScratch();
    deck.render(audio.data(), 256);
    CHECK(std::abs(audio[0]) < 1.0e-6);
    for (int i = 2; i < 512; i += 2) CHECK(std::abs(audio[i] - audio[i - 2]) < 0.01);

    deck.stop();
    deck.beginScratch();
    deck.scratch(2.0);
    render(deck, 768);
    deck.endScratch();
    render(deck, 256, 16); // release fade must also finish with tiny callbacks
    deck.render(audio.data(), 256);
    for (float sample : audio) CHECK(std::abs(sample) < 1.0e-6);
}

void seeking_bounds_and_reloading_do_not_resurrect_old_motion()
{
    gvt::Deck deck;
    deck.loadTrack(tone());
    deck.seekSec(1.0);
    deck.beginScratch();
    deck.scratch(20.0);
    render(deck, 256);
    deck.seekSec(2.0);
    render(deck, 256);
    CHECK(std::abs(deck.positionSec() - 2.0) < 1.0e-9);
    deck.scratch(std::numeric_limits<double>::infinity());
    deck.scratch(std::numeric_limits<double>::quiet_NaN());
    render(deck, 256);
    CHECK(std::abs(deck.positionSec() - 2.0) < 1.0e-9);
    deck.loopAuto(2.0);
    deck.scratch(std::numeric_limits<double>::max());
    render(deck, 16000);
    CHECK(deck.positionSec() < deck.loopEndSec.load());
    CHECK(deck.positionSec() > deck.loopEndSec.load() - 1.0e-6);
    CHECK(deck.loopActive.load());
    deck.scratch(-std::numeric_limits<double>::max());
    render(deck, 16000);
    CHECK(std::abs(deck.positionSec() - deck.loopStartSec.load()) < 1.0e-6);
    CHECK(deck.loopActive.load());
    deck.loadTrack(tone());
    render(deck, 256);
    CHECK(deck.positionSec() == 0.0);
    CHECK(!deck.playing.load());
    deck.beginScratch();
    render(deck, 256);
    CHECK(deck.positionSec() == 0.0);
}

void stem_mix_and_pfl_use_the_same_scratch_trajectory()
{
    gvt::Deck deck;
    auto track = tone();
    deck.loadTrack(track);
    auto stems = std::make_shared<gvt::StemSet>();
    stems->vocals.resize(track->pcm.size());
    stems->melody.resize(track->pcm.size());
    stems->bass.resize(track->pcm.size());
    stems->drums.resize(track->pcm.size());
    for (std::size_t i = 0; i < track->pcm.size(); ++i)
        stems->vocals[i] = static_cast<int16_t>(track->pcm[i] * 32767);
    deck.attachStems(stems);
    deck.stemMelody.store(0.0f);
    deck.stemBass.store(0.0f);
    deck.stemDrums.store(0.0f);
    deck.fader.store(0.25f);
    deck.seekSec(1.0);
    deck.beginScratch();
    std::array<float, 512> out {}, pfl {};
    for (int block = 0; block < 90; ++block) {
        if (block % 3 == 0) deck.scratch(1.6);
        deck.render(out.data(), 256, pfl.data());
        double energy = 0.0;
        for (std::size_t i = 0; i < out.size(); ++i) {
            CHECK(out[i] == pfl[i] * 0.25f);
            energy += pfl[i] * pfl[i];
        }
        if (block > 6) CHECK(energy > 1.0);
    }
    deck.endScratch();
    CHECK(std::abs(deck.positionSec() - 1.48) < 1.0e-9);
}
} // namespace

int main()
{
    const auto start = std::chrono::steady_clock::now();
    sparse_packets_do_not_make_silent_blocks();
    callback_size_does_not_change_the_motion_or_audio();
    fast_scratches_are_band_limited_in_both_directions();
    releases_keep_the_final_hand_position_and_transport_state();
    touches_and_releases_do_not_click_or_leave_a_dc_tail();
    seeking_bounds_and_reloading_do_not_resurrect_old_motion();
    stem_mix_and_pfl_use_the_same_scratch_trajectory();
    std::printf("scratch audio checks: %d failures (%.3fs)\n", failures,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return failures ? 1 : 0;
}

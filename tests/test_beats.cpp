// test_beats — analyzeBeats on a synthetic 128 BPM click track.
// Plain main(), no framework. Returns 0 on pass.

#include "../src/analysis/TrackData.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

int main()
{
    const int    rate = 48000;
    const double bpm  = 128.0;
    const double t0   = 0.25;   // first click (truth for firstBeatSec)
    const double dur  = 60.0;

    // Quiet noise floor + 10 ms click bursts on every beat.
    std::vector<float> mono((size_t)(rate * dur));
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> noise(-0.004f, 0.004f);
    for (float& s : mono) s = noise(rng);

    const int clickLen = rate / 100; // 10 ms
    for (double t = t0; t < dur; t += 60.0 / bpm) {
        const long long start = std::llround(t * rate);
        for (int i = 0; i < clickLen; ++i) {
            const size_t idx = (size_t)(start + i);
            if (idx >= mono.size()) break;
            const float envl = 1.0f - (float)i / (float)clickLen;
            mono[idx] += 0.6f * envl * std::sin(2.0f * (float)M_PI * 1000.0f * (float)i / (float)rate);
        }
    }

    const gvt::BeatAnalysis r = gvt::analyzeBeats(mono.data(), (int64_t)mono.size(), rate);

    if (!r.ok) {
        std::fprintf(stderr, "FAIL: analyzeBeats returned ok=false on click track\n");
        return 1;
    }
    // +/- 1.5 BPM; half/double tempo counts as failure by the same bound.
    if (std::fabs(r.bpm - bpm) > 1.5) {
        std::fprintf(stderr, "FAIL: bpm %.2f, expected %.1f +/- 1.5\n", r.bpm, bpm);
        return 1;
    }
    if (std::fabs(r.firstBeatSec - t0) > 0.060) {
        std::fprintf(stderr, "FAIL: firstBeatSec %.4f, expected %.3f +/- 0.060\n",
                     r.firstBeatSec, t0);
        return 1;
    }

    // Silence must not produce a grid.
    std::vector<float> silence((size_t)(rate * 10), 0.0f);
    if (gvt::analyzeBeats(silence.data(), (int64_t)silence.size(), rate).ok) {
        std::fprintf(stderr, "FAIL: silence reported ok=true\n");
        return 1;
    }
    // Short input must not produce a grid.
    if (gvt::analyzeBeats(mono.data(), rate, rate).ok) {
        std::fprintf(stderr, "FAIL: 1 s input reported ok=true\n");
        return 1;
    }

    std::printf("test_beats OK: bpm=%.2f firstBeat=%.4f s\n", r.bpm, r.firstBeatSec);
    return 0;
}

// test_fingerprint — fingerprint is stable across calls and content-sensitive.
// Plain main(), no framework. Returns 0 on pass.

#include "../src/analysis/TrackData.h"

#include <QString>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
std::vector<float> makeStereoSine(double freq, double seconds)
{
    const int rate = gvt::kSampleRate;
    const int64_t frames = (int64_t)(seconds * rate);
    std::vector<float> pcm((size_t)(frames * 2));
    for (int64_t i = 0; i < frames; ++i) {
        const float v = 0.5f * (float)std::sin(2.0 * M_PI * freq * (double)i / rate);
        pcm[(size_t)(2 * i)]     = v;
        pcm[(size_t)(2 * i + 1)] = v * 0.9f; // slightly different R channel
    }
    return pcm;
}

std::vector<float> makeStructuredSong(double seconds, double pitchScale = 1.0)
{
    const int64_t frames = static_cast<int64_t>(seconds * gvt::kSampleRate);
    std::vector<float> pcm(static_cast<std::size_t>(frames * 2));
    for (int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / gvt::kSampleRate;
        const int phrase = static_cast<int>(t / 2.0) % 4;
        const double carrier = (110.0 + phrase * 73.0) * pitchScale;
        const double pulse = 0.25 + 0.75 * std::pow(
            std::max(0.0, std::sin(2.0 * M_PI * 2.0 * t)), 4.0);
        const float value = static_cast<float>(
            pulse * (0.32 * std::sin(2.0 * M_PI * carrier * t) +
                     0.12 * std::sin(2.0 * M_PI * carrier * 2.7 * t)));
        pcm[static_cast<std::size_t>(2 * i)] = value;
        pcm[static_cast<std::size_t>(2 * i + 1)] = value * 0.91f;
    }
    return pcm;
}

std::vector<float> withSilenceAndGain(const std::vector<float>& source,
                                      double silenceSec, float gain)
{
    const std::size_t silentSamples = static_cast<std::size_t>(
        silenceSec * gvt::kSampleRate * 2);
    std::vector<float> result(silentSamples, 0.0f);
    result.reserve(silentSamples + source.size());
    for (float sample : source) result.push_back(sample * gain);
    return result;
}
} // namespace

int main()
{
    const std::vector<float> a = makeStereoSine(440.0, 35.0);
    const std::vector<float> b = makeStereoSine(523.25, 35.0);

    const QString fpA1 = gvt::computeFingerprint(a.data(), (int64_t)a.size() / 2);
    const QString fpA2 = gvt::computeFingerprint(a.data(), (int64_t)a.size() / 2);
    const QString fpB  = gvt::computeFingerprint(b.data(), (int64_t)b.size() / 2);

    if (!fpA1.startsWith(QStringLiteral("gvfp1:")) || fpA1.size() != 6 + 16) {
        std::fprintf(stderr, "FAIL: bad format: %s\n", qUtf8Printable(fpA1));
        return 1;
    }
    if (fpA1 != fpA2) {
        std::fprintf(stderr, "FAIL: not stable: %s vs %s\n",
                     qUtf8Printable(fpA1), qUtf8Printable(fpA2));
        return 1;
    }
    if (fpA1 == fpB) {
        std::fprintf(stderr, "FAIL: different buffers hashed equal: %s\n",
                     qUtf8Printable(fpA1));
        return 1;
    }

    const auto song = makeStructuredSong(24.0);
    const auto alternateEncode = withSilenceAndGain(song, 0.75, 0.42f);
    const auto differentSong = makeStructuredSong(24.0, 1.37);
    double audibleA = 0.0, audibleAlternate = 0.0;
    const QString structureA = gvt::computeStructureFingerprint(
        song.data(), static_cast<int64_t>(song.size() / 2), &audibleA);
    const QString structureAlternate = gvt::computeStructureFingerprint(
        alternateEncode.data(), static_cast<int64_t>(alternateEncode.size() / 2),
        &audibleAlternate);
    const QString structureDifferent = gvt::computeStructureFingerprint(
        differentSong.data(), static_cast<int64_t>(differentSong.size() / 2));
    if (gvt::structureFingerprintSimilarity(structureA,
                                             structureAlternate) < 0.88) {
        std::fprintf(stderr, "FAIL: same arrangement signature diverged: %s / %s\n",
                     qUtf8Printable(structureA),
                     qUtf8Printable(structureAlternate));
        return 1;
    }
    const double differentSimilarity = gvt::structureFingerprintSimilarity(
        structureA, structureDifferent);
    if (differentSimilarity >= 0.88) {
        std::fprintf(stderr,
                     "FAIL: different arrangement signature matched: %.3f\n",
                     differentSimilarity);
        return 1;
    }
    if (std::fabs(audibleA - audibleAlternate) > 0.3) {
        std::fprintf(stderr, "FAIL: leading silence changed audible duration\n");
        return 1;
    }

    std::printf("test_fingerprint OK: %s / %s\n",
                qUtf8Printable(fpA1), qUtf8Printable(fpB));
    return 0;
}

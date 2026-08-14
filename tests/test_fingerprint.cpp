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

    std::printf("test_fingerprint OK: %s / %s\n",
                qUtf8Printable(fpA1), qUtf8Printable(fpB));
    return 0;
}

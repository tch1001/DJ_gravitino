// KeyAnalyzer — classic Krumhansl-Schmuckler key detection.
// Owned by claude-key. No external DSP deps: decimated mono + Goertzel
// chromagram (C2..B6) + profile correlation, then Camelot-wheel mapping.

#include "KeyAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace gvt {

namespace {

constexpr double kMaxAnalyzeSec = 120.0; // analyze at most the first 2 minutes
constexpr int    kFrameLen      = 4096;  // Goertzel frame (at decimated rate)
constexpr int    kHopLen        = 4096;
constexpr int    kMidiLow       = 48;    // C3 (130.8 Hz) — below this, semitone
                                         // spacing drops under the Goertzel
                                         // bandwidth and the chroma smears
constexpr int    kNumPitches    = 48;    // C3..B6: 4 octaves, every pitch class
                                         // represented an equal number of times
constexpr double kMinCorr       = 0.5;   // below this: report "unknown"
constexpr double kMinSpread     = 1e-4;  // chroma coefficient-of-variation floor

// Krumhansl-Schmuckler tonal-hierarchy profiles (index 0 = tonic).
constexpr std::array<double, 12> kMajorProfile = {
    6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile = {
    6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

// Camelot wheel number per tonic pitch class (C=0 .. B=11).
constexpr std::array<int, 12> kCamelotMajor = {8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1};
constexpr std::array<int, 12> kCamelotMinor = {5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10};

// Conventional spellings (flats for flat-side majors, sharps for sharp minors).
constexpr const char* kMajorNames[12] = {"C",  "Db", "D",  "Eb", "E",  "F",
                                         "F#", "G",  "Ab", "A",  "Bb", "B"};
constexpr const char* kMinorNames[12] = {"C",  "C#", "D",  "Eb", "E",  "F",
                                         "F#", "G",  "G#", "A",  "Bb", "B"};

// Pearson correlation of two length-12 vectors.
double pearson12(const std::array<double, 12>& a, const std::array<double, 12>& b)
{
    double ma = 0.0, mb = 0.0;
    for (int i = 0; i < 12; ++i) { ma += a[i]; mb += b[i]; }
    ma /= 12.0; mb /= 12.0;
    double num = 0.0, da = 0.0, db = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double xa = a[i] - ma, xb = b[i] - mb;
        num += xa * xb; da += xa * xa; db += xb * xb;
    }
    const double den = std::sqrt(da * db);
    if (den < 1e-12) return 0.0;
    return num / den;
}

// Goertzel magnitude of `x[0..n)` at frequency `hz` (rate `fs`).
double goertzelMag(const float* x, int n, double hz, double fs)
{
    const double w = 2.0 * M_PI * hz / fs;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        s0 = (double)x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return std::sqrt(std::max(0.0, power));
}

} // namespace

KeyResult analyzeKey(const float* stereoPcm, int64_t frames, int sampleRate)
{
    KeyResult r;
    if (!stereoPcm || frames <= 0 || sampleRate <= 0) return r;

    // Downmix to mono + decimate to ~11-12 kHz with a boxcar pre-average as a
    // cheap anti-alias (chroma range tops out at B6 ~ 1976 Hz, well below
    // the decimated Nyquist).
    const int decim = std::max(1, sampleRate / 11025); // 48000 -> 4 -> 12000 Hz
    const double fs = (double)sampleRate / (double)decim;
    const int64_t maxInFrames =
        std::min<int64_t>(frames, (int64_t)(kMaxAnalyzeSec * sampleRate));

    std::vector<float> mono;
    mono.reserve((size_t)(maxInFrames / decim + 1));
    for (int64_t i = 0; i + decim <= maxInFrames; i += decim) {
        float acc = 0.0f;
        for (int k = 0; k < decim; ++k)
            acc += 0.5f * (stereoPcm[2 * (i + k)] + stereoPcm[2 * (i + k) + 1]);
        mono.push_back(acc / (float)decim);
    }

    // Need at least a few seconds of audio for a stable chromagram.
    if ((double)mono.size() / fs < 5.0) return r;

    // Precompute Hann window and per-pitch frequencies.
    static_assert(kFrameLen > 0);
    std::vector<float> win((size_t)kFrameLen);
    for (int i = 0; i < kFrameLen; ++i)
        win[(size_t)i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)i /
                                                 (float)(kFrameLen - 1)));
    std::array<double, kNumPitches> freq{};
    for (int p = 0; p < kNumPitches; ++p)
        freq[(size_t)p] = 440.0 * std::pow(2.0, ((kMidiLow + p) - 69) / 12.0);

    // Chromagram: per frame, log-compressed Goertzel magnitude per pitch
    // summed into its pitch class; each frame's chroma is L1-normalized before
    // averaging so loud sections don't dominate, and the log keeps bass-heavy
    // frames from swamping harmonic content.
    std::array<double, 12> chroma{};
    std::vector<float> frame((size_t)kFrameLen);
    int frameCount = 0;
    for (size_t start = 0; start + (size_t)kFrameLen <= mono.size();
         start += (size_t)kHopLen) {
        for (int i = 0; i < kFrameLen; ++i)
            frame[(size_t)i] = mono[start + (size_t)i] * win[(size_t)i];
        std::array<double, 12> fchroma{};
        for (int p = 0; p < kNumPitches; ++p) {
            const int pc = (kMidiLow + p) % 12; // 0 = C
            const double mag =
                goertzelMag(frame.data(), kFrameLen, freq[(size_t)p], fs);
            fchroma[(size_t)pc] += std::log10(1.0 + mag);
        }
        double l1 = 0.0;
        for (double c : fchroma) l1 += c;
        if (l1 > 1e-12)
            for (int i = 0; i < 12; ++i)
                chroma[(size_t)i] += fchroma[(size_t)i] / l1;
        ++frameCount;
    }
    if (frameCount == 0) return r;
    for (double& c : chroma) c /= (double)frameCount;

    // Degenerate input: silence or an essentially flat chroma.
    double mean = 0.0;
    for (double c : chroma) mean += c;
    mean /= 12.0;
    if (mean < 1e-9) return r;
    double var = 0.0;
    for (double c : chroma) var += (c - mean) * (c - mean);
    const double spread = std::sqrt(var / 12.0) / mean; // coefficient of variation
    if (spread < kMinSpread) return r;

    // Correlate against the 24 rotated K-S profiles; best wins.
    double bestCorr = -2.0;
    int bestTonic = -1;
    bool bestMajor = true;
    for (int tonic = 0; tonic < 12; ++tonic) {
        std::array<double, 12> maj{}, min{};
        for (int p = 0; p < 12; ++p) {
            const int deg = (p - tonic + 12) % 12;
            maj[(size_t)p] = kMajorProfile[(size_t)deg];
            min[(size_t)p] = kMinorProfile[(size_t)deg];
        }
        const double cM = pearson12(chroma, maj);
        const double cm = pearson12(chroma, min);
        if (cM > bestCorr) { bestCorr = cM; bestTonic = tonic; bestMajor = true; }
        if (cm > bestCorr) { bestCorr = cm; bestTonic = tonic; bestMajor = false; }
    }
    if (bestTonic < 0 || bestCorr < kMinCorr) return r;

    const int num = bestMajor ? kCamelotMajor[(size_t)bestTonic]
                              : kCamelotMinor[(size_t)bestTonic];
    r.camelotKey = QStringLiteral("%1%2").arg(num).arg(bestMajor
                                                          ? QLatin1Char('B')
                                                          : QLatin1Char('A'));
    r.keyName = bestMajor
                    ? QString::fromLatin1(kMajorNames[bestTonic])
                    : QString::fromLatin1(kMinorNames[bestTonic]) + QLatin1Char('m');
    r.ok = true;
    return r;
}

} // namespace gvt

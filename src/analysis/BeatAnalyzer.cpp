// BeatAnalyzer — fixed-tempo beatgrid estimation (bpm + firstBeatSec).
// Owned by claude-analysis. See docs/ARCHITECTURE.md ("Beatgrid & sync").
//
// Method:
//  1. Onset envelope: log-energy flux (rectified increase) on 512-sample hops.
//  2. Coarse tempo: autocorrelation of the mean-removed envelope, comb-scored
//     over harmonics, searched in 60..180 BPM.
//  3. Octave disambiguation + fine tuning: phase-concentration score (how much
//     onset energy lands in one narrow phase bin of the beat comb), scanned on
//     a fine BPM grid around each octave candidate, preferring 90..180 BPM.
//  4. Phase: dominant phase bin gives the grid; firstBeatSec is the earliest
//     grid position whose local onset peak is strong.

#include "TrackData.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gvt {

namespace {

constexpr int    kHop         = 512;
constexpr double kBpmMin      = 60.0;
constexpr double kBpmMax      = 180.0;
constexpr double kPreferMin   = 80.0;   // octave preference window
constexpr double kPreferMax   = 160.0;  // (pop at 180 reads better as 90)
constexpr double kMaxAnalysisSec = 180.0;

double interpAt(const std::vector<double>& a, double x)
{
    const int i = (int)x;
    if (i < 0 || i + 1 >= (int)a.size()) return 0.0;
    const double f = x - (double)i;
    return a[(size_t)i] * (1.0 - f) + a[(size_t)i + 1] * f;
}

// How concentrated the onset energy is on one phase of a comb with period
// P (in envelope frames). Returns [0..1]; also reports the winning phase
// as a fraction of P.
double phaseScore(const std::vector<double>& env, double P, int nbins, double* phaseFrac)
{
    if (P < 2.0 || env.empty()) return 0.0;
    std::vector<double> hist((size_t)nbins, 0.0);
    double total = 0.0;
    for (size_t i = 0; i < env.size(); ++i) {
        const double v = env[i];
        if (v <= 0.0) continue;
        int b = (int)(std::fmod((double)i, P) / P * nbins);
        if (b >= nbins) b = nbins - 1;
        hist[(size_t)b] += v;
        total += v;
    }
    if (total <= 0.0) return 0.0;
    double best = 0.0;
    int bestB = 0;
    for (int b = 0; b < nbins; ++b) {
        const double v = hist[(size_t)b] + hist[(size_t)((b + 1) % nbins)];
        if (v > best) { best = v; bestB = b; }
    }
    if (phaseFrac) *phaseFrac = std::fmod((bestB + 1.0) / nbins, 1.0);
    return best / total;
}

struct FineResult { double bpm = 0.0; double score = 0.0; };

// Scan a fine BPM grid around `center` maximizing the phase-concentration.
FineResult refineBpm(const std::vector<double>& env, double envRate, double center)
{
    FineResult r;
    const double lo = std::max(kBpmMin, center - 4.0);
    const double hi = std::min(kBpmMax, center + 4.0);
    for (double bpm = lo; bpm <= hi + 1e-9; bpm += 0.05) {
        const double P = 60.0 / bpm * envRate;
        const double s = phaseScore(env, P, 32, nullptr);
        if (s > r.score) { r.score = s; r.bpm = bpm; }
    }
    return r;
}

} // namespace

BeatAnalysis analyzeBeats(const float* mono, int64_t n, int sampleRate)
{
    BeatAnalysis result;
    if (!mono || sampleRate <= 0) return result;
    if (n < (int64_t)sampleRate * 5) return result;          // too short

    // ---- 1. Onset envelope: log-energy flux per hop -------------------------
    const double envRate = (double)sampleRate / kHop;
    int64_t nHops = n / kHop;
    const int64_t maxHops = (int64_t)(kMaxAnalysisSec * envRate);
    if (nHops > maxHops) nHops = maxHops;

    std::vector<double> env((size_t)nHops, 0.0);
    double peak = 0.0, prevLog = -9.0;
    for (int64_t h = 0; h < nHops; ++h) {
        double e = 0.0;
        const float* p = mono + h * kHop;
        for (int i = 0; i < kHop; ++i) {
            const double s = p[i];
            e += s * s;
            const double a = std::fabs(s);
            if (a > peak) peak = a;
        }
        const double le = std::log10(1e-9 + e / kHop);
        if (h > 0) env[(size_t)h] = std::max(0.0, le - prevLog);
        prevLog = le;
    }

    double envTotal = 0.0;
    for (double v : env) envTotal += v;
    if (peak < 1e-4 || envTotal <= 1e-6) return result;      // silence

    // Light 3-point smoothing.
    std::vector<double> smooth(env.size(), 0.0);
    for (size_t i = 1; i + 1 < env.size(); ++i)
        smooth[i] = 0.25 * env[i - 1] + 0.5 * env[i] + 0.25 * env[i + 1];

    // ---- 2. Coarse tempo via comb-scored autocorrelation --------------------
    const size_t nE = smooth.size();
    double mean = 0.0;
    for (double v : smooth) mean += v;
    mean /= (double)nE;
    std::vector<double> zc(nE);
    for (size_t i = 0; i < nE; ++i) zc[i] = smooth[i] - mean;

    const int lagMin = std::max(2, (int)std::floor(envRate * 60.0 / kBpmMax));
    const int lagMax = (int)std::ceil(envRate * 60.0 / kBpmMin);
    const int lagTop = std::min((int)nE / 2, lagMax * 4 + 2);
    if (lagMin >= lagTop) return result;

    std::vector<double> ac((size_t)lagTop, 0.0);
    for (int L = 1; L < lagTop; ++L) {
        double s = 0.0;
        for (size_t i = 0; i + (size_t)L < nE; ++i) s += zc[i] * zc[i + (size_t)L];
        ac[(size_t)L] = std::max(0.0, s / (double)(nE - (size_t)L));
    }

    double bestComb = -1.0;
    int bestLag = lagMin;
    for (int L = lagMin; L <= std::min(lagMax, lagTop - 1); ++L) {
        const double s = ac[(size_t)L]
                       + 0.75 * interpAt(ac, 2.0 * L)
                       + 0.50 * interpAt(ac, 3.0 * L)
                       + 0.25 * interpAt(ac, 4.0 * L);
        if (s > bestComb) { bestComb = s; bestLag = L; }
    }
    if (bestComb <= 0.0) return result;
    const double coarseBpm = 60.0 * envRate / bestLag;

    // ---- 3. Octave candidates + fine tuning ---------------------------------
    std::vector<double> candidates;
    for (double c : {coarseBpm, coarseBpm * 2.0, coarseBpm * 0.5}) {
        if (c < kBpmMin - 2.0 || c > kBpmMax + 2.0) continue;
        c = std::clamp(c, kBpmMin, kBpmMax);
        bool dup = false;
        for (double d : candidates) if (std::fabs(d - c) < 1.0) dup = true;
        if (!dup) candidates.push_back(c);
    }
    if (candidates.empty()) candidates.push_back(std::clamp(coarseBpm, kBpmMin, kBpmMax));

    FineResult best, bestPreferred;
    for (double c : candidates) {
        const FineResult f = refineBpm(env, envRate, c);
        if (f.score > best.score) best = f;
        if (f.bpm >= kPreferMin && f.bpm <= kPreferMax && f.score > bestPreferred.score)
            bestPreferred = f;
    }
    FineResult chosen = best;
    if (bestPreferred.bpm > 0.0 && bestPreferred.score >= 0.65 * best.score)
        chosen = bestPreferred;                              // prefer 80..160
    if (chosen.bpm <= 0.0 || chosen.score < 0.09) return result;  // no clear beat

    const double bpm = chosen.bpm;
    const double P = 60.0 / bpm * envRate;                   // beat period, env frames

    // ---- 4. Phase + earliest strong onset on the grid -----------------------
    double phaseFrac = 0.0;
    phaseScore(env, P, 64, &phaseFrac);
    const double p0 = phaseFrac * P;                         // first grid pos, frames

    const int win = std::max(2, (int)std::lround(P * 0.12));
    struct Beat { double strength; int64_t frame; };
    std::vector<Beat> beats;
    for (double t = p0; t < (double)nE; t += P) {
        const int64_t c = (int64_t)std::llround(t);
        double s = -1.0;
        int64_t at = c;
        for (int64_t i = std::max<int64_t>(0, c - win);
             i <= std::min<int64_t>((int64_t)nE - 1, c + win); ++i)
            if (env[(size_t)i] > s) { s = env[(size_t)i]; at = i; }
        beats.push_back({std::max(0.0, s), at});
    }
    if (beats.empty()) return result;

    std::vector<double> strengths;
    strengths.reserve(beats.size());
    for (const Beat& b : beats) strengths.push_back(b.strength);
    std::sort(strengths.begin(), strengths.end(), std::greater<double>());
    double ref = 0.0;
    const size_t topN = std::max<size_t>(1, strengths.size() / 4);
    for (size_t i = 0; i < topN; ++i) ref += strengths[i];
    ref /= (double)topN;

    int64_t firstFrame = beats.front().frame;
    for (const Beat& b : beats)
        if (b.strength >= 0.35 * ref) { firstFrame = b.frame; break; }

    result.bpm = bpm;
    result.firstBeatSec = (double)(firstFrame * kHop) / (double)sampleRate;
    result.ok = true;
    return result;
}

} // namespace gvt

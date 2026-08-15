// Internal helpers shared between src/analysis and src/library.
// Not a pinned interface — owned by claude-analysis.
#pragma once
#include <QString>
#include <vector>

namespace gvt::detail {

// Decode an MP3 to interleaved stereo f32 at kSampleRate (48 kHz).
// Returns false and sets *error on failure.
bool decodeMp3Stereo48k(const QString& path, std::vector<float>& pcmOut, QString* error);

// Read title/artist/album via TagLib; title falls back to the filename stem.
void readTags(const QString& path, QString& title, QString& artist, QString& album);

// Mono max-abs per 512-frame bin, 0..1 (for waveform overview drawing).
std::vector<float> computeOverviewPeaks(const std::vector<float>& stereoPcm);

// Serato-style band overviews: per 512-frame bin peak abs of the mono signal
// split into low (<200 Hz), mid (200–2000 Hz), high (>2000 Hz) via one-pole
// lowpasses. All three vectors are sized exactly like computeOverviewPeaks'
// result and normalized by one shared global max so relative band balance is
// preserved. Silent input -> all zeros, never NaN. Called both on fresh
// analysis and on library cache hits (bands are recomputed from PCM, not
// stored in the JSON cache).
void computeBandOverviews(const std::vector<float>& stereoPcm,
                          std::vector<float>& low,
                          std::vector<float>& mid,
                          std::vector<float>& high);

// 0.5*(L+R) mixdown.
std::vector<float> monoMixdown(const std::vector<float>& stereoPcm);

} // namespace gvt::detail

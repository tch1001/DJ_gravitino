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

// 0.5*(L+R) mixdown.
std::vector<float> monoMixdown(const std::vector<float>& stereoPcm);

} // namespace gvt::detail

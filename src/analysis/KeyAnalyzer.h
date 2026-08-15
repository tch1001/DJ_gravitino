// KeyAnalyzer — musical key detection (Krumhansl-Schmuckler over a Goertzel
// chromagram) with Camelot-wheel mapping. Owned by claude-key.
// Not a pinned interface.
#pragma once
#include <QString>
#include <cstdint>

namespace gvt {

struct KeyResult {
    QString camelotKey; // "8B" = C major, "8A" = A minor, ... empty if not ok
    QString keyName;    // "C", "Am", "F#", "Ebm", ...
    bool    ok = false;
};

// Analyze interleaved stereo f32 PCM at `sampleRate` (up to the first ~120 s
// are used). Returns ok=false (empty strings) for input that is too short,
// silent, or whose key correlation is degenerate. Thread-safe, no globals.
KeyResult analyzeKey(const float* stereoPcm, int64_t frames, int sampleRate);

} // namespace gvt

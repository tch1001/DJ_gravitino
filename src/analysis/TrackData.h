// PINNED INTERFACE — see docs/ARCHITECTURE.md before changing.
#pragma once
#include <QString>
#include <memory>
#include <vector>

namespace gvt {

constexpr int kSampleRate = 48000; // engine + all decoded tracks run at 48 kHz

struct SavedLoopSlot {
    double startSec = -1.0;
    double endSec = -1.0;
    QString label;

    bool isSet() const noexcept
    {
        return startSec >= 0.0 && endSec > startSec;
    }
};

// A fully decoded, analyzed track resident in memory.
struct TrackData {
    QString filePath;
    QString title, artist, album;
    QString isrc, musicBrainzRecording;
    double  durationSec = 0.0;
    double  audibleDurationSec = 0.0;

    // Interleaved stereo f32 at kSampleRate.
    std::vector<float> pcm;
    int64_t frameCount() const { return (int64_t)pcm.size() / 2; }

    // Fixed-tempo beatgrid.
    double bpm = 0.0;
    double firstBeatSec = 0.0;
    double beatAtSec(double sec) const { return (sec - firstBeatSec) * bpm / 60.0; }
    double secAtBeat(double beat) const { return firstBeatSec + beat * 60.0 / bpm; }
    // Local catalog alignment: canonical arrangement beat = analyzed asset
    // beat + this constant. It is never written into the audio file.
    double canonicalBeatOffset = 0.0;
    double canonicalBeatAtSec(double sec) const
    {
        return beatAtSec(sec) + canonicalBeatOffset;
    }
    double secAtCanonicalBeat(double beat) const
    {
        return secAtBeat(beat - canonicalBeatOffset);
    }

    // "gvfp1:<16 hex>" — see docs/TRANSITION_FORMAT.md.
    QString fingerprint;
    // Encode-tolerant whole-arrangement evidence (currently gvsf2). Unlike
    // gvfp1, this is based on normalized temporal/spectral block features
    // after trimming silence.
    QString structureFingerprint;
    // Exact bytes identify one asset, never the abstract song arrangement.
    QString assetSha256;
    QString songId;

    // Musical key in Camelot notation ("8A" = A minor, "8B" = C major...);
    // empty if detection failed. keyName is the traditional name ("Am").
    QString camelotKey, keyName;

    // Mono peak per ~512-frame bin, 0..1, for waveform drawing.
    std::vector<float> overviewPeaks;

    // Per ~512-frame bin band energies, 0..1, same bin count as overviewPeaks.
    // low <200 Hz, mid 200–2000 Hz, high >2000 Hz — for Serato-style colored
    // waveforms. All three share one normalization (band balance preserved).
    std::vector<float> overviewLow, overviewMid, overviewHigh;

    // Hot cue positions in seconds; <0 = unset. Index 0..7.
    double hotCues[8] = {-1,-1,-1,-1,-1,-1,-1,-1};

    // Serato-style saved loop slots. Empty slots have negative bounds.
    SavedLoopSlot savedLoops[8];
};
using TrackDataPtr = std::shared_ptr<TrackData>;

// Separated stems for one track: interleaved stereo int16 at kSampleRate,
// all four the same frame count as the track's pcm (padded/truncated).
// Produced asynchronously by StemSeparator; attached to a Deck for playback.
struct StemSet {
    std::vector<int16_t> vocals, melody, bass, drums;
    int64_t frameCount() const { return (int64_t)vocals.size() / 2; }
};
using StemSetPtr = std::shared_ptr<StemSet>;

// Blocking: decode + tags + fingerprint + BPM analysis. Returns nullptr and
// sets *error on failure. Thread-safe; call from a worker thread.
TrackDataPtr loadAndAnalyzeTrack(const QString& audioPath, QString* error);

// Exposed for unit tests: analyze mono 48k samples -> bpm & first beat.
struct BeatAnalysis { double bpm = 0.0; double firstBeatSec = 0.0; bool ok = false; };
BeatAnalysis analyzeBeats(const float* mono, int64_t n, int sampleRate);

QString computeFingerprint(const float* stereoPcm, int64_t frames); // "gvfp1:..."
QString computeStructureFingerprint(const float* stereoPcm, int64_t frames,
                                    double* audibleDurationSec = nullptr);
double structureFingerprintSimilarity(const QString& a, const QString& b);

} // namespace gvt

// Sample-clock sampler shared by live playback, editor audition and set export.
// Prepared source audio is immutable; only the audio callback advances voices.
#pragma once
#include "../analysis/TrackData.h"
#include "../transitions/TonePlay.h"
#include <atomic>
#include <memory>

namespace gvt {
class TonePlayProcessor {
  public:
    TonePlayProcessor();
    ~TonePlayProcessor();
    bool prepare(const TrackData &source, const TonePlayPattern &pattern, double startSeconds,
                 double endSeconds, double anchorSeconds, QString *error);
    void clear(); // GUI thread, drains readers before freeing prepared audio
    void enable(bool value) noexcept { enabled_.store(value); }
    double beat() const noexcept { return publishedBeat_.load(); }
    // Overwrites stereo tone output and per-frame original-audio gain.
    void render(float *tone, float *originalGain, int frames, double positionSec, bool playing,
                double tempoRatio) noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
    std::atomic<bool> swapping_{false}, enabled_{false};
    std::atomic<int> readers_{0};
    std::atomic<double> publishedBeat_{-1.0};
};
} // namespace gvt

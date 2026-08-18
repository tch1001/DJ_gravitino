#pragma once

namespace gvt {

// Pure value model for editing a fixed-tempo beat grid. The editor pins the
// grid line nearest referenceSecond as its anchor so BPM corrections do not
// make that chosen musical moment drift in time.
class BeatGridEditor final {
public:
    static constexpr double kMinBpm = 20.0;
    static constexpr double kMaxBpm = 400.0;

    BeatGridEditor(double bpm, double firstBeatSec,
                   double referenceSecond) noexcept;

    double bpm() const noexcept { return bpm_; }
    double firstBeatSec() const noexcept { return firstBeatSec_; }
    double anchorSecond() const noexcept { return anchorSecond_; }
    bool hasValidGrid() const noexcept;

    static bool isValidBpm(double bpm) noexcept;

    // Make the supplied playhead position beat zero/downbeat and the pinned
    // anchor. This is allowed before a valid BPM has been entered.
    bool setDownbeatAt(double currentSecond) noexcept;

    // Shift the complete grid in seconds. Positive is later; negative is
    // earlier. The pinned anchor moves by the same amount.
    bool nudgeSeconds(double deltaSeconds) noexcept;

    // Tempo edits preserve the pinned anchor exactly. Out-of-range or
    // non-finite requests fail without changing the model.
    bool halveBpm() noexcept;
    bool doubleBpm() noexcept;
    bool setBpm(double bpm) noexcept;

private:
    double bpm_ = 0.0;
    double firstBeatSec_ = 0.0;
    double anchorSecond_ = 0.0;
};

} // namespace gvt

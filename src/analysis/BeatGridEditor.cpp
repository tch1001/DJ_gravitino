#include "BeatGridEditor.h"

#include <cmath>

namespace gvt {

BeatGridEditor::BeatGridEditor(double bpm, double firstBeatSec,
                               double referenceSecond) noexcept
    : bpm_(isValidBpm(bpm) ? bpm : 0.0),
      firstBeatSec_(std::isfinite(firstBeatSec) ? firstBeatSec : 0.0),
      anchorSecond_(std::isfinite(referenceSecond) && referenceSecond >= 0.0
                        ? referenceSecond
                        : 0.0)
{
    // Pin the existing grid line nearest the reference. A later tempo edit
    // therefore keeps an actual beat/downbeat line stationary, rather than
    // pinning an arbitrary point between beats.
    if (isValidBpm(bpm_)) {
        const double beat = std::round(
            (anchorSecond_ - firstBeatSec_) * bpm_ / 60.0);
        const double snapped = firstBeatSec_ + beat * 60.0 / bpm_;
        if (std::isfinite(snapped))
            anchorSecond_ = snapped;
    }
}

bool BeatGridEditor::isValidBpm(double bpm) noexcept
{
    return std::isfinite(bpm) && bpm >= kMinBpm && bpm <= kMaxBpm;
}

bool BeatGridEditor::hasValidGrid() const noexcept
{
    return isValidBpm(bpm_) && std::isfinite(firstBeatSec_) &&
           std::isfinite(anchorSecond_);
}

bool BeatGridEditor::setDownbeatAt(double currentSecond) noexcept
{
    if (!std::isfinite(currentSecond) || currentSecond < 0.0)
        return false;
    firstBeatSec_ = currentSecond;
    anchorSecond_ = currentSecond;
    return true;
}

bool BeatGridEditor::nudgeSeconds(double deltaSeconds) noexcept
{
    if (!std::isfinite(deltaSeconds))
        return false;
    const double first = firstBeatSec_ + deltaSeconds;
    const double anchor = anchorSecond_ + deltaSeconds;
    if (!std::isfinite(first) || !std::isfinite(anchor))
        return false;
    firstBeatSec_ = first;
    anchorSecond_ = anchor;
    return true;
}

bool BeatGridEditor::halveBpm() noexcept
{
    if (!isValidBpm(bpm_))
        return false;
    return setBpm(bpm_ * 0.5);
}

bool BeatGridEditor::doubleBpm() noexcept
{
    if (!isValidBpm(bpm_))
        return false;
    return setBpm(bpm_ * 2.0);
}

bool BeatGridEditor::setBpm(double bpm) noexcept
{
    if (!isValidBpm(bpm) || !std::isfinite(firstBeatSec_) ||
        !std::isfinite(anchorSecond_))
        return false;

    // Choose the new integer beat number nearest the pinned line, then solve
    // firstBeatSec so that this line remains exactly at anchorSecond_. Using
    // the new tempo naturally preserves phase for half/double corrections.
    const double anchorBeat = std::round(
        (anchorSecond_ - firstBeatSec_) * bpm / 60.0);
    const double first = anchorSecond_ - anchorBeat * 60.0 / bpm;
    if (!std::isfinite(anchorBeat) || !std::isfinite(first))
        return false;

    bpm_ = bpm;
    firstBeatSec_ = first;
    return true;
}

} // namespace gvt

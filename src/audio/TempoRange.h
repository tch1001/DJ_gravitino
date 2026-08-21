#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace gvt {

// Serato DJ Pro's standard virtual-deck pitch ranges. The DDJ-FLX4 exposes
// the host's range cycle on SHIFT + BEAT SYNC.
inline constexpr std::array<double, 3> kSeratoTempoRanges {
    0.08, 0.16, 0.50};

inline double closestSeratoTempoRange(double requested) noexcept
{
    if (!std::isfinite(requested))
        return kSeratoTempoRanges.front();
    return *std::min_element(
        kSeratoTempoRanges.begin(), kSeratoTempoRanges.end(),
        [requested](double left, double right) {
            return std::fabs(left - requested) <
                   std::fabs(right - requested);
        });
}

inline double nextSeratoTempoRange(double current) noexcept
{
    const double selected = closestSeratoTempoRange(current);
    for (std::size_t index = 0; index < kSeratoTempoRanges.size(); ++index) {
        if (std::fabs(kSeratoTempoRanges[index] - selected) < 1.0e-9)
            return kSeratoTempoRanges[
                (index + 1U) % kSeratoTempoRanges.size()];
    }
    return kSeratoTempoRanges.front();
}

// A physical/software pitch-fader position is normalized to -1..+1, then
// expanded by the selected range. This keeps the center exact at every range.
inline double tempoRatioFromFaderPosition(double position,
                                          double range) noexcept
{
    if (!std::isfinite(position)) position = 0.0;
    return 1.0 + std::clamp(position, -1.0, 1.0) *
                     closestSeratoTempoRange(range);
}

inline double tempoFaderPositionFromRatio(double ratio,
                                          double range) noexcept
{
    if (!std::isfinite(ratio)) return 0.0;
    return std::clamp(
        (ratio - 1.0) / closestSeratoTempoRange(range), -1.0, 1.0);
}

} // namespace gvt

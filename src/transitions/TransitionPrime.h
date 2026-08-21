#pragma once

#include <cmath>

namespace gvt {

// Whether a currently active forward loop still permits transport to reach a
// future transition entry. If playback is before LOOP IN, every beat up to
// LOOP OUT remains reachable; only after entering the loop is reachability
// restricted to the loop region itself.
inline bool activeLoopCanReachTransitionEntry(
    double currentBeat, double entryBeat,
    double loopStartBeat, double loopEndBeat) noexcept
{
    if (!std::isfinite(currentBeat) || !std::isfinite(entryBeat) ||
        !std::isfinite(loopStartBeat) || !std::isfinite(loopEndBeat) ||
        loopEndBeat <= loopStartBeat)
        return true;

    if (currentBeat < loopStartBeat)
        return entryBeat < loopEndBeat;
    return entryBeat >= loopStartBeat && entryBeat < loopEndBeat;
}

} // namespace gvt

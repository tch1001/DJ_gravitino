#pragma once

#include <algorithm>
#include <cmath>

namespace gvt {

enum class SetupToleranceField {
    Other,
    Bpm,
    Volume,
    Eq,
};

struct TransitionSetupTolerances {
    bool closeEnough = false;
    double bpm = 0.5;       // absolute BPM
    double volume = 0.05;   // normalized 0..1 controls
    double eq = 0.05;       // normalized 0..1 controls
};

inline double transitionSetupTolerance(
    SetupToleranceField field, double strictTolerance,
    const TransitionSetupTolerances& configured) noexcept
{
    strictTolerance = std::max(0.0, strictTolerance);
    if (!configured.closeEnough)
        return strictTolerance;
    switch (field) {
    case SetupToleranceField::Bpm:
        return std::max(strictTolerance, configured.bpm);
    case SetupToleranceField::Volume:
        return std::max(strictTolerance, configured.volume);
    case SetupToleranceField::Eq:
        return std::max(strictTolerance, configured.eq);
    case SetupToleranceField::Other:
        return strictTolerance;
    }
    return strictTolerance;
}

inline bool transitionSetupValueMatches(
    double actual, double wanted, SetupToleranceField field,
    double strictTolerance,
    const TransitionSetupTolerances& configured) noexcept
{
    return std::isfinite(actual) && std::isfinite(wanted) &&
           std::fabs(actual - wanted) <= transitionSetupTolerance(
               field, strictTolerance, configured);
}

} // namespace gvt

#include "transitions/TransitionTolerance.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        ++failures; \
    } \
} while (0)
}

int main()
{
    using namespace gvt;
    TransitionSetupTolerances tolerance;

    // Checkbox off retains the original strict margins.
    CHECK(transitionSetupValueMatches(
        120.04, 120.0, SetupToleranceField::Bpm, 0.05, tolerance));
    CHECK(!transitionSetupValueMatches(
        120.20, 120.0, SetupToleranceField::Bpm, 0.05, tolerance));
    CHECK(!transitionSetupValueMatches(
        0.53, 0.50, SetupToleranceField::Volume, 0.015, tolerance));

    tolerance.closeEnough = true;
    tolerance.bpm = 0.75;
    tolerance.volume = 0.06;
    tolerance.eq = 0.08;
    CHECK(transitionSetupValueMatches(
        120.70, 120.0, SetupToleranceField::Bpm, 0.05, tolerance));
    CHECK(transitionSetupValueMatches(
        0.55, 0.50, SetupToleranceField::Volume, 0.015, tolerance));
    CHECK(transitionSetupValueMatches(
        0.57, 0.50, SetupToleranceField::Eq, 0.015, tolerance));
    CHECK(!transitionSetupValueMatches(
        0.59, 0.50, SetupToleranceField::Eq, 0.015, tolerance));

    // Unrelated continuous/discrete setup checks never become looser.
    CHECK(!transitionSetupValueMatches(
        1.03, 1.0, SetupToleranceField::Other, 0.015, tolerance));
    CHECK(!transitionSetupValueMatches(
        std::nan(""), 1.0, SetupToleranceField::Bpm, 0.05, tolerance));

    if (failures != 0)
        return 1;
    std::printf("test_transition_tolerance: strict and close-enough margins passed\n");
    return 0;
}

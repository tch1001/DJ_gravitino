#include "performance/PerformancePads.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "CHECK failed: %s:%d: %s\n", __FILE__, __LINE__, #x); \
    std::exit(1); } } while (0)

using namespace gvt;

int main()
{
    for (int mode = 0; mode < (int)PerformancePadMode::Count; ++mode) {
        const auto m = static_cast<PerformancePadMode>(mode);
        CHECK(performancePadModeKey(m)[0] != '\0');
        CHECK(performancePadModeLabel(m)[0] != '\0');
        for (int pad = 0; pad < kPerformancePadCount; ++pad) {
            const auto assignment = defaultPerformancePadAssignment(m, pad);
            CHECK(!assignment.label.empty());
            CHECK(performancePadActionIsSupported(assignment.action) ==
                  (m == PerformancePadMode::HotCue ||
                   m == PerformancePadMode::PadFx1 ||
                   m == PerformancePadMode::PadFx2 ||
                   m == PerformancePadMode::BeatJump ||
                   m == PerformancePadMode::BeatLoop ||
                   m == PerformancePadMode::Sampler ||
                   m == PerformancePadMode::SavedLoop));
        }
    }

    CHECK(!performancePadModeIsShifted(PerformancePadMode::HotCue));
    CHECK(performancePadModeIsShifted(PerformancePadMode::Keyboard));
    CHECK(performancePadModeIsShifted(PerformancePadMode::PadFx2));
    CHECK(performancePadModeIsShifted(PerformancePadMode::BeatLoop));
    CHECK(performancePadModeIsShifted(PerformancePadMode::KeyShift));
    CHECK(!performancePadModeIsShifted(PerformancePadMode::SavedLoop));
    CHECK(std::string(performancePadModeLabel(PerformancePadMode::Sampler)) ==
          "CUSTOM");

    const auto savedLoop = defaultPerformancePadAssignment(
        PerformancePadMode::SavedLoop, 5);
    CHECK(savedLoop.action == PerformancePadAction::SavedLoop);
    CHECK(savedLoop.label == "L6");

    auto invalid = defaultPerformancePadAssignment(
        PerformancePadMode::PadFx1, 0);
    invalid.fxType = 99;
    invalid.fxWet = -5.0;
    invalid.fxBeats = std::numeric_limits<double>::infinity();
    invalid.label.clear();
    const auto fixed = sanitizePerformancePadAssignment(
        PerformancePadMode::PadFx1, 0, invalid);
    CHECK(fixed.action == PerformancePadAction::FxHold);
    CHECK(fixed.fxType == 2);
    CHECK(fixed.fxWet == 0.0);
    CHECK(fixed.fxBeats == 0.25);
    CHECK(!fixed.label.empty());

    auto jump = defaultPerformancePadAssignment(
        PerformancePadMode::BeatJump, 0);
    jump.value = 0.0;
    jump.action = PerformancePadAction::SamplerSlot;
    jump = sanitizePerformancePadAssignment(
        PerformancePadMode::BeatJump, 0, jump);
    CHECK(jump.action == PerformancePadAction::BeatJump);
    CHECK(std::abs(jump.value + 16.0) < 1e-9);

    std::printf("test_performance_pads: mode defaults and validation passed\n");
    return 0;
}

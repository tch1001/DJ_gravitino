#include "midi/Flx4TutorialMap.h"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)
}

int main()
{
    using namespace gvt;

    for (int pad = 0; pad < 8; ++pad) {
        const auto id = static_cast<ControlId>(
            static_cast<int>(ControlId::HotCue1) + pad);
        const auto mapped = flx4TutorialMapping(id, 1.0);
        CHECK(mapped.has_value());
        if (mapped) {
            CHECK(mapped->surface == Flx4SurfaceControl::PerformancePad);
            CHECK(mapped->padMode == Flx4PadMode::HotCue);
            CHECK(mapped->pad == pad);
        }
    }

    const auto jumpBack8 = flx4TutorialMapping(ControlId::BeatJump, -8.0);
    CHECK(jumpBack8.has_value());
    if (jumpBack8) {
        CHECK(jumpBack8->padMode == Flx4PadMode::BeatJump);
        CHECK(jumpBack8->pad == 6);
    }
    CHECK(!flx4TutorialMapping(ControlId::BeatJump, 3.0));
    CHECK(flx4TutorialMapping(ControlId::LoopAuto, 4.0));
    CHECK(!flx4TutorialMapping(ControlId::LoopAuto, 8.0));
    CHECK(flx4TutorialMapping(ControlId::Stop, 1.0)->surface ==
          Flx4SurfaceControl::PlayPause);
    CHECK(flx4TutorialMapping(ControlId::FxOn, 1.0)->needsFxAssignment);
    CHECK(!flx4TutorialMapping(ControlId::Quantize, 1.0)->needsFxAssignment);
    CHECK(!flx4TutorialMapping(ControlId::StemVocals, 0.0));

    if (failures) return 1;
    std::printf("test_flx4_tutorial: surface mappings passed\n");
    return 0;
}

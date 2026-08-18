#include "midi/SoftTakeover.h"

#include <cmath>
#include <cstdio>
#include <vector>

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
    SoftTakeover takeover;

    // A known mismatch arms. Unrelated FLX4 input remains frozen, moving the
    // knob toward target is consumed, and crossing the target releases it.
    takeover.rememberHardware({0, ControlId::EqHigh, 0.1});
    takeover.arm({{0, ControlId::EqHigh, 0.7}});
    CHECK(takeover.active());
    CHECK(takeover.pending().size() == 1);
    CHECK(!takeover.acceptHardware({0, ControlId::Play, 1.0}));
    bool changed = false;
    CHECK(!takeover.acceptHardware({0, ControlId::EqHigh, 0.5}, &changed));
    CHECK(!changed && takeover.active());
    CHECK(!takeover.acceptHardware({0, ControlId::EqHigh, 0.71}, &changed));
    CHECK(changed && !takeover.active());
    CHECK(takeover.acceptHardware({0, ControlId::EqHigh, 0.72}));

    // Controls already matching at arm time do not create false alerts.
    takeover.rememberHardware({kNoDeck, ControlId::Crossfader, 0.5});
    takeover.arm({{kNoDeck, ControlId::Crossfader, 0.505}});
    CHECK(!takeover.active());

    // Unknown physical position stays pending until the user moves through
    // the desired point; the pickup event itself is still consumed.
    takeover.clearHardware();
    takeover.arm({{1, ControlId::Tempo, 0.98}});
    CHECK(takeover.active());
    CHECK(!takeover.pending().front().hardwareKnown);
    CHECK(!takeover.acceptHardware({1, ControlId::Tempo, 1.04}));
    CHECK(takeover.active());
    CHECK(!takeover.acceptHardware({1, ControlId::Tempo, 0.9795}, &changed));
    CHECK(changed && !takeover.active());
    CHECK(takeover.acceptHardware({1, ControlId::Play, 1.0}));

    // A virtual/software edit while frozen becomes the new authoritative
    // pickup target instead of leaving hardware matched to stale replay state.
    takeover.rememberHardware({0, ControlId::EqLow, 0.2});
    takeover.arm({{0, ControlId::EqLow, 0.8}});
    CHECK(takeover.retarget({0, ControlId::EqLow, 0.6}));
    CHECK(std::fabs(takeover.pending().front().targetValue - 0.6) < 1e-9);
    CHECK(!takeover.acceptHardware({0, ControlId::EqLow, 0.61}, &changed));
    CHECK(changed && !takeover.active());

    if (failures) return 1;
    std::printf("test_soft_takeover: pickup gating passed\n");
    return 0;
}

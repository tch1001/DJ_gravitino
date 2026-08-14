// TransitionPlayer pure-logic tests: curve interpolation and schedule
// building (src/transitions/PlayerMath.h). The full player needs a live
// AudioEngine and is exercised by ./gravitino --selftest instead.
// Returns 0 on pass. Owner: claude-transitions.
#include <cmath>
#include <cstdio>
#include "transitions/PlayerMath.h"

namespace {

int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

} // namespace

int main() {
    using namespace gvt;

    // ---- smoothstep -------------------------------------------------------
    CHECK(near(smoothstep01(0.0), 0.0));
    CHECK(near(smoothstep01(1.0), 1.0));
    CHECK(near(smoothstep01(0.5), 0.5));
    CHECK(near(smoothstep01(-2.0), 0.0));  // clamped
    CHECK(near(smoothstep01(3.0), 1.0));   // clamped
    CHECK(smoothstep01(0.25) < 0.25);      // slow start
    CHECK(smoothstep01(0.75) > 0.75);      // slow end
    for (double t = 0.0; t < 1.0; t += 0.1) // monotonic
        CHECK(smoothstep01(t) <= smoothstep01(t + 0.1) + 1e-12);

    // ---- curveValue -------------------------------------------------------
    CHECK(near(curveValue(Curve::Step, 0.3, 0.0, 1.0), 1.0));   // step snaps
    CHECK(near(curveValue(Curve::Linear, 0.0, 0.2, 0.8), 0.2));
    CHECK(near(curveValue(Curve::Linear, 0.5, 0.2, 0.8), 0.5));
    CHECK(near(curveValue(Curve::Linear, 1.0, 0.2, 0.8), 0.8));
    CHECK(near(curveValue(Curve::Linear, 1.7, 0.2, 0.8), 0.8)); // clamped
    CHECK(near(curveValue(Curve::SCurve, 0.5, 0.0, 1.0), 0.5));
    CHECK(curveValue(Curve::SCurve, 0.25, 0.0, 1.0) <
          curveValue(Curve::Linear, 0.25, 0.0, 1.0));
    CHECK(near(curveValue(Curve::SCurve, 1.0, 1.0, 0.0), 0.0)); // descending

    // ---- buildSchedule: glide starts chain per (role, control) ------------
    std::vector<GvtEvent> ev = {
        {0.0, Role::ToDeck, ControlId::EqLow, 0.8, Curve::Step},
        {4.0, Role::Mixer, ControlId::Crossfader, 0.5, Curve::SCurve},
        {8.0, Role::ToDeck, ControlId::EqLow, 0.2, Curve::Linear},
        {8.0, Role::ToDeck, ControlId::Play, 1.0, Curve::Step},
        {12.0, Role::Mixer, ControlId::Crossfader, 1.0, Curve::Linear},
    };
    auto engineNow = [](Role r, ControlId id) {
        return (r == Role::Mixer && id == ControlId::Crossfader) ? 0.1 : 0.5;
    };
    auto sched = buildSchedule(ev, engineNow);
    CHECK(sched.size() == 5);

    // [0] step event: no glide, start == due.
    CHECK(near(sched[0].startBeat, 0.0));
    CHECK(!scheduledIsGlide(sched[0]));

    // [1] xfader glide with no prior key event: starts at beat 0 from the
    // engine's current value (0.1).
    CHECK(near(sched[1].startBeat, 0.0));
    CHECK(near(sched[1].startValue, 0.1));
    CHECK(scheduledIsGlide(sched[1]));
    CHECK(near(glideValueAt(sched[1], 2.0), 0.1 + (0.5 - 0.1) * 0.5)); // midpoint
    CHECK(near(glideValueAt(sched[1], 4.0), 0.5));                     // arrival

    // [2] eq_low glide chains from the previous eq_low event (0.8 @ beat 0).
    CHECK(near(sched[2].startBeat, 0.0));
    CHECK(near(sched[2].startValue, 0.8));
    CHECK(near(glideValueAt(sched[2], 4.0), 0.5)); // halfway 0.8 -> 0.2

    // [3] trigger: never a glide.
    CHECK(!scheduledIsGlide(sched[3]));

    // [4] second xfader glide chains from the first (0.5 @ beat 4).
    CHECK(near(sched[4].startBeat, 4.0));
    CHECK(near(sched[4].startValue, 0.5));
    CHECK(near(glideValueAt(sched[4], 8.0), 0.75));
    CHECK(near(glideValueAt(sched[4], 12.0), 1.0));

    // Before its start a glide clamps to the start value.
    CHECK(near(glideValueAt(sched[4], 2.0), 0.5));

    // Zero-length glide (due == start) snaps to the target.
    {
        std::vector<GvtEvent> z = {
            {3.0, Role::FromDeck, ControlId::Fader, 1.0, Curve::Step},
            {3.0, Role::FromDeck, ControlId::Fader, 0.0, Curve::Linear},
        };
        auto zs = buildSchedule(z, engineNow);
        CHECK(!scheduledIsGlide(zs[1]));
        CHECK(near(glideValueAt(zs[1], 3.0), 0.0));
    }

    if (failures) {
        std::fprintf(stderr, "test_player: %d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_player: all checks passed\n");
    return 0;
}

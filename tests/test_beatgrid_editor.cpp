#include "analysis/BeatGridEditor.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        ++failures; \
    } \
} while (0)

bool near(double actual, double expected, double tolerance = 1e-9)
{
    return std::fabs(actual - expected) <= tolerance;
}
}

int main()
{
    using gvt::BeatGridEditor;

    BeatGridEditor grid(120.0, 0.25, 10.28);
    // Constructor pins the existing line nearest the supplied reference.
    CHECK(near(grid.anchorSecond(), 10.25));
    CHECK(grid.hasValidGrid());

    CHECK(grid.setBpm(121.25));
    CHECK(near(grid.bpm(), 121.25));
    const double anchoredBeat =
        (grid.anchorSecond() - grid.firstBeatSec()) * grid.bpm() / 60.0;
    CHECK(near(anchoredBeat, std::round(anchoredBeat)));
    CHECK(near(grid.anchorSecond(), 10.25));

    BeatGridEditor half(120.0, 0.25, 8.25);
    CHECK(half.halveBpm());
    CHECK(near(half.bpm(), 60.0));
    CHECK(near(half.firstBeatSec(), 0.25));
    CHECK(near(half.anchorSecond(), 8.25));

    BeatGridEditor doubled(100.0, 0.4, 5.2);
    CHECK(doubled.doubleBpm());
    CHECK(near(doubled.bpm(), 200.0));
    CHECK(near(doubled.firstBeatSec(), 0.4));
    CHECK(near(doubled.anchorSecond(), 5.2));

    CHECK(doubled.setDownbeatAt(3.75));
    CHECK(near(doubled.firstBeatSec(), 3.75));
    CHECK(near(doubled.anchorSecond(), 3.75));
    CHECK(doubled.nudgeSeconds(-0.012));
    CHECK(near(doubled.firstBeatSec(), 3.738));
    CHECK(near(doubled.anchorSecond(), 3.738));

    const double unchangedBpm = doubled.bpm();
    const double unchangedFirst = doubled.firstBeatSec();
    CHECK(!doubled.setBpm(BeatGridEditor::kMaxBpm + 0.01));
    CHECK(!doubled.setBpm(std::numeric_limits<double>::quiet_NaN()));
    CHECK(!doubled.nudgeSeconds(std::numeric_limits<double>::infinity()));
    CHECK(!doubled.setDownbeatAt(-1.0));
    CHECK(near(doubled.bpm(), unchangedBpm));
    CHECK(near(doubled.firstBeatSec(), unchangedFirst));

    BeatGridEditor tooSlow(20.0, 0.0, 0.0);
    BeatGridEditor tooFast(400.0, 0.0, 0.0);
    CHECK(!tooSlow.halveBpm());
    CHECK(!tooFast.doubleBpm());

    if (failures != 0)
        return 1;
    std::printf("test_beatgrid_editor: beat-grid corrections passed\n");
    return 0;
}

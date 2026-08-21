#include "audio/AudioEngine.h"
#include "audio/TempoRange.h"
#include "control/ControlBus.h"

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

bool near(double left, double right)
{
    return std::fabs(left - right) < 1.0e-9;
}
}

int main()
{
    using namespace gvt;

    CHECK(near(closestSeratoTempoRange(0.08), 0.08));
    CHECK(near(closestSeratoTempoRange(0.15), 0.16));
    CHECK(near(nextSeratoTempoRange(0.08), 0.16));
    CHECK(near(nextSeratoTempoRange(0.16), 0.50));
    CHECK(near(nextSeratoTempoRange(0.50), 0.08));
    CHECK(near(tempoRatioFromFaderPosition(-1.0, 0.50), 0.50));
    CHECK(near(tempoRatioFromFaderPosition(1.0, 0.50), 1.50));
    CHECK(near(tempoFaderPositionFromRatio(1.08, 0.08), 1.0));

    ControlBus bus;
    AudioEngine engine(&bus);
    Deck& deck = engine.deck(0);
    deck.tempoRatio.store(1.30);

    // Choosing or cycling the fader range must never change live BPM/ratio.
    bus.dispatch({0, ControlId::TempoRange, 0.16}, Origin::Ui);
    CHECK(near(deck.tempoRange.load(), 0.16));
    CHECK(near(deck.tempoRatio.load(), 1.30));
    bus.dispatch({0, ControlId::TempoRange, 0.0}, Origin::Midi);
    CHECK(near(deck.tempoRange.load(), 0.50));
    CHECK(near(deck.tempoRatio.load(), 1.30));
    bus.dispatch({0, ControlId::TempoRange, 0.0}, Origin::Midi);
    CHECK(near(deck.tempoRange.load(), 0.08));

    if (failures) return 1;
    std::printf("test_tempo_range: Serato 8/16/50%% cycle preserves tempo\n");
    return 0;
}

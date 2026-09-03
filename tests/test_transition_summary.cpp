#include "transitions/TransitionEventSummary.h"

#include <algorithm>
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

const gvt::HumanTransitionRow* rowContaining(
    const std::vector<gvt::HumanTransitionRow>& rows, int eventIndex)
{
    const int row = gvt::humanTransitionRowForEventIndex(rows, eventIndex);
    return row >= 0 ? &rows[static_cast<std::size_t>(row)] : nullptr;
}

std::vector<int> coveredIndices(
    const std::vector<gvt::HumanTransitionRow>& rows)
{
    std::vector<int> result;
    for (const auto& row : rows)
        result.insert(result.end(), row.eventIndices.begin(),
                      row.eventIndices.end());
    std::sort(result.begin(), result.end());
    return result;
}
}

int main()
{
    using namespace gvt;

    GvtFile overlapping;
    overlapping.initialFrom.captured = true;
    overlapping.initialTo.captured = true;
    overlapping.events = {
        {9.0, Role::ToDeck, ControlId::Fader, 0.0, Curve::Step},
        {10.0, Role::FromDeck, ControlId::EqLow, 0.5, Curve::Step},
        {12.0, Role::FromDeck, ControlId::EqLow, 0.25, Curve::Linear},
        {13.0, Role::ToDeck, ControlId::Fader, 0.5, Curve::Linear},
        {16.0, Role::FromDeck, ControlId::EqLow, 0.0, Curve::SCurve},
        {17.0, Role::ToDeck, ControlId::Fader, 1.0, Curve::Linear},
        {20.0, Role::FromDeck, ControlId::EqLow, 0.5, Curve::Step},
        {22.0, Role::FromDeck, ControlId::EqLow, 0.2, Curve::Linear},
    };
    const std::vector<HumanTransitionRow> overlapRows =
        humanTransitionRows(overlapping);
    CHECK(overlapRows.size() == 2);
    const HumanTransitionRow* first = rowContaining(overlapRows, 1);
    CHECK(first != nullptr);
    CHECK(first && first->outgoing.has_value());
    CHECK(first && first->incoming.has_value());
    CHECK(first && first->outgoing->eventIndices ==
                       std::vector<int>({1, 2, 4}));
    CHECK(first && first->incoming->eventIndices ==
                       std::vector<int>({0, 3, 5}));
    CHECK(first && first->outgoing->startBeat == 10.0);
    CHECK(first && first->outgoing->endBeat == 16.0);
    const HumanTransitionRow* second = rowContaining(overlapRows, 6);
    CHECK(second != nullptr);
    CHECK(second && second->outgoing.has_value());
    CHECK(second && !second->incoming.has_value());
    CHECK(second && second->outgoing->eventIndices ==
                        std::vector<int>({6, 7}));

    GvtFile launch;
    launch.events = {
        {1.0, Role::ToDeck, ControlId::HotCue3, 1.0, Curve::Step},
        {1.2, Role::Mixer, ControlId::Crossfader, 0.2, Curve::Step},
        {1.5, Role::FromDeck, ControlId::EqLow, 0.4, Curve::Step},
        {2.0, Role::ToDeck, ControlId::Play, 1.0, Curve::Step},
        {2.3, Role::FromDeck, ControlId::EqLow, 0.2, Curve::Linear},
        {3.0, Role::ToDeck, ControlId::HotCue3, 0.0, Curve::Step},
        {4.0, Role::ToDeck, ControlId::LoopExit, 1.0, Curve::Step},
    };
    const std::vector<HumanTransitionRow> launchRows =
        humanTransitionRows(launch);
    const HumanTransitionRow* launchRow = rowContaining(launchRows, 0);
    CHECK(launchRow != nullptr);
    CHECK(launchRow && launchRow->incoming.has_value());
    CHECK(launchRow && launchRow->incoming->kind ==
                           HumanActionKind::HotCueStart);
    CHECK(launchRow && launchRow->incoming->hotCuePad == 3);
    CHECK(launchRow && launchRow->incoming->eventIndices ==
                           std::vector<int>({0, 3, 5}));
    CHECK(humanTransitionRowForEventIndex(launchRows, 3) ==
          humanTransitionRowForEventIndex(launchRows, 0));
    const HumanTransitionRow* loopRow = rowContaining(launchRows, 6);
    CHECK(loopRow && loopRow->incoming->kind == HumanActionKind::Event);
    const HumanTransitionRow* mixerRow = rowContaining(launchRows, 1);
    CHECK(mixerRow && mixerRow->shared.has_value());
    CHECK(coveredIndices(launchRows) ==
          std::vector<int>({0, 1, 2, 3, 4, 5, 6}));

    GvtFile conflict;
    conflict.events = {
        {1.0, Role::ToDeck, ControlId::HotCue1, 1.0, Curve::Step},
        {1.5, Role::ToDeck, ControlId::HotCue2, 1.0, Curve::Step},
        {2.0, Role::ToDeck, ControlId::Play, 1.0, Curve::Step},
        {2.5, Role::ToDeck, ControlId::HotCue1, 0.0, Curve::Step},
    };
    const std::vector<HumanTransitionRow> conflictRows =
        humanTransitionRows(conflict);
    const HumanTransitionRow* conflictPress = rowContaining(conflictRows, 0);
    CHECK(conflictPress && conflictPress->incoming->kind ==
                               HumanActionKind::Event);
    CHECK(humanTransitionRowForEventIndex(conflictRows, 0) !=
          humanTransitionRowForEventIndex(conflictRows, 2));

    GvtFile firstGlide;
    firstGlide.initialTo.captured = true;
    firstGlide.initialTo.fader = 0.1;
    firstGlide.events = {
        {8.0, Role::ToDeck, ControlId::Fader, 1.0, Curve::Linear},
    };
    const auto glideActions = humanTransitionActions(firstGlide);
    CHECK(glideActions.size() == 1);
    CHECK(glideActions[0].kind == HumanActionKind::Continuous);
    CHECK(glideActions[0].startBeat == 0.0);
    CHECK(glideActions[0].startValue == 0.1);
    CHECK(glideActions[0].endBeat == 8.0);
    CHECK(glideActions[0].endValue == 1.0);

    // Pairing is deterministic and chooses the greatest actual overlap.
    GvtFile bestOverlap;
    bestOverlap.events = {
        {0.0, Role::FromDeck, ControlId::Fader, 0.0, Curve::Step},
        {0.0, Role::ToDeck, ControlId::EqLow, 0.0, Curve::Step},
        {2.0, Role::ToDeck, ControlId::EqHigh, 0.0, Curve::Step},
        {5.0, Role::ToDeck, ControlId::EqLow, 0.5, Curve::Linear},
        {10.0, Role::FromDeck, ControlId::Fader, 1.0, Curve::Linear},
        {10.0, Role::ToDeck, ControlId::EqHigh, 0.5, Curve::Linear},
    };
    const auto bestRows = humanTransitionRows(bestOverlap);
    const HumanTransitionRow* pairedRow = rowContaining(bestRows, 0);
    CHECK(pairedRow && pairedRow->incoming.has_value());
    CHECK(pairedRow && pairedRow->incoming->control == ControlId::EqHigh);
    CHECK(humanTransitionRowForEventIndex(bestRows, 0) ==
          humanTransitionRowForEventIndex(bestRows, 5));
    CHECK(humanTransitionRowForEventIndex(bestRows, 0) !=
          humanTransitionRowForEventIndex(bestRows, 1));

    // The synthetic beat-zero row prepares the first action. At an action's
    // exact beat the overlay moves to that row; equal-beat actions introduce
    // no fake interval, and the final row fills through the recorded end.
    const std::vector<double> rowBeats {0.0, 2.0, 2.0, 5.0};
    CHECK(transitionSequenceProgressAt(rowBeats, -0.1, 9.0).row == -1);
    const TransitionSequenceProgress startProgress =
        transitionSequenceProgressAt(rowBeats, 1.0, 9.0);
    CHECK(startProgress.row == 0);
    CHECK(std::fabs(startProgress.fraction - 0.5) < 1.0e-9);
    const TransitionSequenceProgress simultaneous =
        transitionSequenceProgressAt(rowBeats, 2.0, 9.0);
    CHECK(simultaneous.row == 2);
    CHECK(std::fabs(simultaneous.fraction) < 1.0e-9);
    const TransitionSequenceProgress finalProgress =
        transitionSequenceProgressAt(rowBeats, 7.0, 9.0);
    CHECK(finalProgress.row == 3);
    CHECK(std::fabs(finalProgress.fraction - 0.5) < 1.0e-9);
    CHECK(transitionSequenceProgressAt(rowBeats, 9.0, 9.0).fraction == 1.0);

    if (failures) return 1;
    std::puts("test_transition_summary: human grouping passed");
    return 0;
}

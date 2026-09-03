#pragma once

#include "Transition.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace gvt {

// The replay keeps every recorded automation checkpoint, but a DJ-facing
// sequence should describe each continuous move rather than expose its sample
// density. Crossfader automation is one mixer move; EQ automation is grouped
// independently by role and band so simultaneous outgoing/incoming moves keep
// their own start and end rows.
inline bool transitionSummaryIsEq(ControlId control) noexcept
{
    return control == ControlId::EqLow || control == ControlId::EqMid ||
           control == ControlId::EqHigh;
}

inline std::vector<int> summarizedTransitionEventIndices(const GvtFile& file)
{
    using Key = std::pair<int, int>;
    std::map<Key, std::pair<int, int>> endpoints;

    const auto keyFor = [](const GvtEvent& event) -> Key {
        if (event.control == ControlId::Crossfader)
            return {-1, static_cast<int>(ControlId::Crossfader)};
        return {static_cast<int>(event.role),
                static_cast<int>(event.control)};
    };

    for (int i = 0; i < static_cast<int>(file.events.size()); ++i) {
        const GvtEvent& event = file.events[static_cast<std::size_t>(i)];
        if (event.control != ControlId::Crossfader &&
            !transitionSummaryIsEq(event.control))
            continue;
        auto [it, inserted] = endpoints.emplace(keyFor(event),
                                                std::pair<int, int>{i, i});
        if (!inserted) it->second.second = i;
    }

    std::vector<int> indices;
    indices.reserve(file.events.size());
    for (int i = 0; i < static_cast<int>(file.events.size()); ++i) {
        const GvtEvent& event = file.events[static_cast<std::size_t>(i)];
        if (event.control == ControlId::Crossfader ||
            transitionSummaryIsEq(event.control)) {
            const auto it = endpoints.find(keyFor(event));
            if (it != endpoints.end() && i != it->second.first &&
                i != it->second.second)
                continue;
        }
        indices.push_back(i);
    }
    return indices;
}

inline std::pair<int, int> transitionSummaryEndpoints(
    const GvtFile& file, int eventIndex)
{
    if (eventIndex < 0 || eventIndex >= static_cast<int>(file.events.size()))
        return {-1, -1};
    const GvtEvent& wanted = file.events[static_cast<std::size_t>(eventIndex)];
    if (wanted.control != ControlId::Crossfader &&
        !transitionSummaryIsEq(wanted.control))
        return {eventIndex, eventIndex};

    int first = -1;
    int last = -1;
    for (int i = 0; i < static_cast<int>(file.events.size()); ++i) {
        const GvtEvent& candidate = file.events[static_cast<std::size_t>(i)];
        const bool same = wanted.control == ControlId::Crossfader
            ? candidate.control == ControlId::Crossfader
            : candidate.control == wanted.control &&
                  candidate.role == wanted.role;
        if (!same) continue;
        if (first < 0) first = i;
        last = i;
    }
    return {first, last};
}

enum class HumanActionKind {
    Event,
    Continuous,
    HotCueStart,
};

// A semantic action derived from one or more raw .gvt events. Text stays in
// the UI layer so this model remains deterministic, locale-free, and easy to
// test. eventIndices is also the bridge back to Tutorial's raw event stream.
struct HumanTransitionAction {
    Role role = Role::Mixer;
    ControlId control = ControlId::Count;
    HumanActionKind kind = HumanActionKind::Event;
    double startBeat = 0.0;
    double endBeat = 0.0;
    double startValue = 0.0;
    double endValue = 0.0;
    Curve endCurve = Curve::Step;
    int hotCuePad = 0; // 1..8 for HotCueStart, otherwise 0
    std::vector<int> eventIndices;
};

// Human mode is a two-lane timeline. Overlapping outgoing/incoming continuous
// moves may share a row; mixer actions use shared. At most one action occupies
// each lane so row selection remains unambiguous.
struct HumanTransitionRow {
    std::optional<HumanTransitionAction> outgoing;
    std::optional<HumanTransitionAction> incoming;
    std::optional<HumanTransitionAction> shared;
    std::vector<int> eventIndices;
    double startBeat = 0.0;
    double endBeat = 0.0;
};

inline bool humanTransitionIsHotCue(ControlId control) noexcept
{
    return control >= ControlId::HotCue1 && control <= ControlId::HotCue8;
}

inline bool humanTransitionIsSavedLoop(ControlId control) noexcept
{
    return control >= ControlId::SavedLoop1 &&
           control <= ControlId::SavedLoop8;
}

inline bool humanTransitionLaunchConflict(ControlId control) noexcept
{
    return control == ControlId::Play || control == ControlId::Stop ||
           control == ControlId::Cue || control == ControlId::Load ||
           humanTransitionIsHotCue(control) ||
           humanTransitionIsSavedLoop(control);
}

inline double humanTransitionInitialValue(const GvtFile& file, Role role,
                                          ControlId control,
                                          double fallback) noexcept
{
    if (role == Role::Mixer)
        return control == ControlId::Crossfader &&
                       file.initialMixerCaptured
                   ? file.initialCrossfader
                   : fallback;

    const GvtInitialState& state = role == Role::FromDeck
                                       ? file.initialFrom : file.initialTo;
    if (!state.captured) return fallback;
    switch (control) {
    case ControlId::Tempo: return state.tempoRatio;
    case ControlId::Fader: return state.fader;
    case ControlId::Trim: return state.trim;
    case ControlId::EqLow: return state.eqLow;
    case ControlId::EqMid: return state.eqMid;
    case ControlId::EqHigh: return state.eqHigh;
    case ControlId::Filter: return state.filter;
    case ControlId::Quantize: return state.quantize ? 1.0 : 0.0;
    case ControlId::FxType: return static_cast<double>(state.fxType);
    case ControlId::FxOn: return state.fxOn ? 1.0 : 0.0;
    case ControlId::FxWet: return state.fxWet;
    case ControlId::FxBeats: return state.fxBeats;
    case ControlId::StemVocals: return state.stemVocals;
    case ControlId::StemMelody: return state.stemMelody;
    case ControlId::StemBass: return state.stemBass;
    case ControlId::StemDrums: return state.stemDrums;
    default: return fallback;
    }
}

inline std::vector<HumanTransitionAction> humanTransitionActions(
    const GvtFile& file)
{
    std::vector<HumanTransitionAction> actions;
    std::vector<bool> consumed(file.events.size(), false);

    // Fold the familiar hold-hot-cue, latch with PLAY, release gesture into a
    // single launch instruction. Events on the other deck or mixer do not
    // disturb the pattern; conflicting transport on this deck does.
    for (int i = 0; i < static_cast<int>(file.events.size()); ++i) {
        const GvtEvent& press = file.events[static_cast<std::size_t>(i)];
        if (consumed[static_cast<std::size_t>(i)] ||
            press.role == Role::Mixer ||
            !humanTransitionIsHotCue(press.control) || press.value < 0.5)
            continue;

        int playIndex = -1;
        int releaseIndex = -1;
        for (int j = i + 1; j < static_cast<int>(file.events.size()); ++j) {
            const GvtEvent& candidate =
                file.events[static_cast<std::size_t>(j)];
            if (candidate.role != press.role) continue;
            if (candidate.control == press.control) {
                if (candidate.value < 0.5 && playIndex >= 0)
                    releaseIndex = j;
                break;
            }
            if (candidate.control == ControlId::Play && playIndex < 0) {
                playIndex = j;
                continue;
            }
            if (humanTransitionLaunchConflict(candidate.control)) break;
        }
        if (playIndex < 0 || releaseIndex < 0) continue;

        HumanTransitionAction action;
        action.role = press.role;
        action.control = press.control;
        action.kind = HumanActionKind::HotCueStart;
        action.startBeat = press.beat;
        action.endBeat =
            file.events[static_cast<std::size_t>(releaseIndex)].beat;
        action.startValue = press.value;
        action.endValue =
            file.events[static_cast<std::size_t>(releaseIndex)].value;
        action.hotCuePad = static_cast<int>(press.control) -
                           static_cast<int>(ControlId::HotCue1) + 1;
        action.eventIndices = {i, playIndex, releaseIndex};
        actions.push_back(action);
        consumed[static_cast<std::size_t>(i)] = true;
        consumed[static_cast<std::size_t>(playIndex)] = true;
        consumed[static_cast<std::size_t>(releaseIndex)] = true;
    }

    // Continuous curves are contiguous within their own role/control stream,
    // not within the globally interleaved event list. A Step explicitly starts
    // a new segment; later Linear/S-Curve endpoints extend that segment.
    using Key = std::pair<int, int>;
    std::map<Key, std::vector<int>> streams;
    for (int i = 0; i < static_cast<int>(file.events.size()); ++i) {
        if (consumed[static_cast<std::size_t>(i)]) continue;
        const GvtEvent& event = file.events[static_cast<std::size_t>(i)];
        if (!controlIsTrigger(event.control))
            streams[{static_cast<int>(event.role),
                     static_cast<int>(event.control)}].push_back(i);
    }

    for (const auto& [key, stream] : streams) {
        (void)key;
        std::size_t begin = 0;
        while (begin < stream.size()) {
            std::size_t end = begin + 1;
            while (end < stream.size() &&
                   file.events[static_cast<std::size_t>(stream[end])].curve !=
                       Curve::Step)
                ++end;

            const GvtEvent& first =
                file.events[static_cast<std::size_t>(stream[begin])];
            const GvtEvent& last =
                file.events[static_cast<std::size_t>(stream[end - 1])];
            HumanTransitionAction action;
            action.role = first.role;
            action.control = first.control;
            action.kind = end - begin > 1 || first.curve != Curve::Step
                              ? HumanActionKind::Continuous
                              : HumanActionKind::Event;
            action.startBeat = first.curve == Curve::Step ? first.beat : 0.0;
            action.endBeat = last.beat;
            action.startValue = first.curve == Curve::Step
                                    ? first.value
                                    : humanTransitionInitialValue(
                                          file, first.role, first.control,
                                          first.value);
            action.endValue = last.value;
            action.endCurve = last.curve;
            for (std::size_t p = begin; p < end; ++p) {
                action.eventIndices.push_back(stream[p]);
                consumed[static_cast<std::size_t>(stream[p])] = true;
            }
            actions.push_back(std::move(action));
            begin = end;
        }
    }

    // Triggers and isolated controls remain literal, one event per action.
    for (int i = 0; i < static_cast<int>(file.events.size()); ++i) {
        if (consumed[static_cast<std::size_t>(i)]) continue;
        const GvtEvent& event = file.events[static_cast<std::size_t>(i)];
        HumanTransitionAction action;
        action.role = event.role;
        action.control = event.control;
        action.startBeat = event.beat;
        action.endBeat = event.beat;
        action.startValue = event.value;
        action.endValue = event.value;
        action.endCurve = event.curve;
        action.eventIndices = {i};
        actions.push_back(std::move(action));
    }

    std::stable_sort(
        actions.begin(), actions.end(),
        [](const HumanTransitionAction& a,
           const HumanTransitionAction& b) {
            if (a.startBeat != b.startBeat) return a.startBeat < b.startBeat;
            return a.eventIndices.front() < b.eventIndices.front();
        });
    return actions;
}

inline std::vector<HumanTransitionRow> humanTransitionRows(
    const GvtFile& file)
{
    const std::vector<HumanTransitionAction> actions =
        humanTransitionActions(file);
    std::vector<bool> paired(actions.size(), false);

    struct Candidate {
        std::size_t outgoing = 0;
        std::size_t incoming = 0;
        double overlap = 0.0;
        double start = 0.0;
    };
    std::vector<Candidate> candidates;
    for (std::size_t a = 0; a < actions.size(); ++a) {
        if (actions[a].role != Role::FromDeck ||
            actions[a].kind != HumanActionKind::Continuous)
            continue;
        for (std::size_t b = 0; b < actions.size(); ++b) {
            if (actions[b].role != Role::ToDeck ||
                actions[b].kind != HumanActionKind::Continuous)
                continue;
            const double overlap =
                std::min(actions[a].endBeat, actions[b].endBeat) -
                std::max(actions[a].startBeat, actions[b].startBeat);
            if (overlap < -0.0005) continue;
            candidates.push_back(
                {a, b, std::max(0.0, overlap),
                 std::min(actions[a].startBeat, actions[b].startBeat)});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         if (a.overlap != b.overlap)
                             return a.overlap > b.overlap;
                         if (a.start != b.start) return a.start < b.start;
                         return std::tie(a.outgoing, a.incoming) <
                                std::tie(b.outgoing, b.incoming);
                     });

    std::vector<HumanTransitionRow> rows;
    const auto finalize = [](HumanTransitionRow& row) {
        std::sort(row.eventIndices.begin(), row.eventIndices.end());
        row.eventIndices.erase(
            std::unique(row.eventIndices.begin(), row.eventIndices.end()),
            row.eventIndices.end());
    };
    for (const Candidate& candidate : candidates) {
        if (paired[candidate.outgoing] || paired[candidate.incoming]) continue;
        HumanTransitionRow row;
        row.outgoing = actions[candidate.outgoing];
        row.incoming = actions[candidate.incoming];
        row.startBeat = std::min(row.outgoing->startBeat,
                                 row.incoming->startBeat);
        row.endBeat = std::max(row.outgoing->endBeat,
                               row.incoming->endBeat);
        row.eventIndices = row.outgoing->eventIndices;
        row.eventIndices.insert(row.eventIndices.end(),
                                row.incoming->eventIndices.begin(),
                                row.incoming->eventIndices.end());
        finalize(row);
        rows.push_back(std::move(row));
        paired[candidate.outgoing] = true;
        paired[candidate.incoming] = true;
    }

    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (paired[i]) continue;
        HumanTransitionRow row;
        if (actions[i].role == Role::FromDeck)
            row.outgoing = actions[i];
        else if (actions[i].role == Role::ToDeck)
            row.incoming = actions[i];
        else
            row.shared = actions[i];
        row.startBeat = actions[i].startBeat;
        row.endBeat = actions[i].endBeat;
        row.eventIndices = actions[i].eventIndices;
        finalize(row);
        rows.push_back(std::move(row));
    }

    std::stable_sort(rows.begin(), rows.end(),
                     [](const HumanTransitionRow& a,
                        const HumanTransitionRow& b) {
                         if (a.startBeat != b.startBeat)
                             return a.startBeat < b.startBeat;
                         return a.eventIndices.front() < b.eventIndices.front();
                     });
    return rows;
}

inline int humanTransitionRowForEventIndex(
    const std::vector<HumanTransitionRow>& rows, int eventIndex) noexcept
{
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const auto& indices = rows[static_cast<std::size_t>(row)].eventIndices;
        if (std::find(indices.begin(), indices.end(), eventIndex) !=
            indices.end())
            return row;
    }
    return -1;
}

struct TransitionSequenceProgress {
    int row = -1;
    double fraction = 0.0;
};

// Resolve the row whose instruction has just been reached and how far time
// has advanced toward the next distinct row. rowBeats includes the synthetic
// non-actionable start marker at beat zero. Equal-beat actions are simultaneous
// and therefore do not manufacture a countdown interval between themselves.
inline TransitionSequenceProgress transitionSequenceProgressAt(
    const std::vector<double>& rowBeats, double beatsIn,
    double beatsTotal) noexcept
{
    if (rowBeats.empty() || !std::isfinite(beatsIn) || beatsIn < rowBeats[0])
        return {};

    const auto reached = std::upper_bound(rowBeats.begin(), rowBeats.end(),
                                          beatsIn);
    const int row = static_cast<int>(
        std::distance(rowBeats.begin(), reached)) - 1;
    if (row < 0) return {};

    const double start = rowBeats[static_cast<std::size_t>(row)];
    std::size_t next = static_cast<std::size_t>(row + 1);
    while (next < rowBeats.size() &&
           rowBeats[next] <= start + 0.0005)
        ++next;
    const double finish = next < rowBeats.size()
                              ? rowBeats[next]
                              : std::max(start, beatsTotal);
    const double fraction = finish <= start + 0.0005
                                ? 1.0
                                : std::clamp((beatsIn - start) /
                                                 (finish - start),
                                             0.0, 1.0);
    return {row, fraction};
}

} // namespace gvt

// INTERNAL header (not pinned) — pure beat-clock/interpolation logic for
// TransitionPlayer, factored out header-only so tests/test_player.cpp can
// exercise it without linking the audio engine. Owner: claude-transitions.
#pragma once
#include <algorithm>
#include <functional>
#include <vector>
#include "Transition.h"

namespace gvt {

inline double smoothstep01(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Value of a glide at normalized progress t in [0,1] from v0 to v1.
// Step snaps to v1 (a step "glide" has no intermediate values).
inline double curveValue(Curve c, double t, double v0, double v1) {
    switch (c) {
        case Curve::Step:   return v1;
        case Curve::Linear: return v0 + (v1 - v0) * std::clamp(t, 0.0, 1.0);
        case Curve::SCurve: return v0 + (v1 - v0) * smoothstep01(t);
    }
    return v1;
}

// One event with its resolved glide start: where (in beats) and from what
// value the interpolation begins. Triggers and Step events glide from
// themselves (startBeat == beat), i.e. they just fire at `beat`.
struct ScheduledEvent {
    GvtEvent e;
    double startBeat = 0.0;   // beat the glide starts (== e.beat for steps)
    double startValue = 0.0;  // value the glide starts from
};

inline bool scheduledIsGlide(const ScheduledEvent& s) {
    return !controlIsTrigger(s.e.control) && s.e.curve != Curve::Step &&
           s.e.beat > s.startBeat;
}

// Build the absolute schedule from beat-sorted events. `currentValue`
// supplies the engine's value for a (role, control) at arm time, used when a
// glide has no earlier event for the same key. Glides with no predecessor
// start at beat 0 (the transition anchor).
inline std::vector<ScheduledEvent> buildSchedule(
    const std::vector<GvtEvent>& events,
    const std::function<double(Role, ControlId)>& currentValue) {
    std::vector<ScheduledEvent> out;
    out.reserve(events.size());
    for (const GvtEvent& e : events) {
        ScheduledEvent s;
        s.e = e;
        s.startBeat = e.beat;
        s.startValue = e.value;
        if (!controlIsTrigger(e.control)) {
            bool found = false;
            for (auto it = out.rbegin(); it != out.rend(); ++it) {
                if (it->e.role == e.role && it->e.control == e.control &&
                    !controlIsTrigger(it->e.control)) {
                    s.startBeat = it->e.beat;
                    s.startValue = it->e.value;
                    found = true;
                    break;
                }
            }
            if (!found) {
                s.startBeat = 0.0;
                s.startValue = currentValue ? currentValue(e.role, e.control)
                                            : e.value;
            }
            if (e.curve == Curve::Step) s.startBeat = e.beat; // no glide
        }
        out.push_back(s);
    }
    return out;
}

// Glide sample at absolute transition beat `rel` (beats past anchor).
inline double glideValueAt(const ScheduledEvent& s, double rel) {
    if (s.e.beat <= s.startBeat) return s.e.value;
    const double t = (rel - s.startBeat) / (s.e.beat - s.startBeat);
    return curveValue(s.e.curve, t, s.startValue, s.e.value);
}

} // namespace gvt

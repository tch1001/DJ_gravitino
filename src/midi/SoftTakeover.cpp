#include "SoftTakeover.h"

#include <algorithm>
#include <cmath>

namespace gvt {

bool SoftTakeover::supports(const ControlEvent& event) noexcept
{
    if (!std::isfinite(event.value)) return false;
    if (event.id == ControlId::Crossfader)
        return event.deck == kNoDeck;
    if (event.deck < 0 || event.deck >= 2) return false;
    switch (event.id) {
    case ControlId::Tempo:
    case ControlId::Fader:
    case ControlId::Trim:
    case ControlId::EqLow:
    case ControlId::EqMid:
    case ControlId::EqHigh:
    case ControlId::Filter:
        return true;
    default:
        return false;
    }
}

double SoftTakeover::tolerance(ControlId control) noexcept
{
    // Tempo is a ratio over the FLX4's +/-8% range; 0.001 is a visible 0.1%
    // pitch-fader step. Normalized mixer controls get a 1.2% pickup window.
    return control == ControlId::Tempo ? 0.001 : 0.012;
}

void SoftTakeover::clear()
{
    pending_.clear();
}

void SoftTakeover::clearHardware()
{
    hardware_.clear();
    pending_.clear();
}

void SoftTakeover::rememberHardware(const ControlEvent& event)
{
    if (!supports(event)) return;
    hardware_[keyFor(event)] = HardwareValue {event.value, true};
}

void SoftTakeover::arm(const std::vector<ControlEvent>& targets)
{
    pending_.clear();
    for (const ControlEvent& target : targets) {
        if (!supports(target)) continue;
        const Key key = keyFor(target);
        const auto hardware = hardware_.find(key);
        if (hardware != hardware_.end() && hardware->second.known &&
            std::fabs(hardware->second.value - target.value) <=
                tolerance(target.id)) {
            continue;
        }
        pending_[key] = target.value;
    }
}

bool SoftTakeover::retarget(const ControlEvent& target)
{
    if (!supports(target)) return false;
    const Key key = keyFor(target);
    const auto existing = pending_.find(key);
    const bool wasPending = existing != pending_.end();
    const double oldTarget = wasPending ? existing->second : 0.0;
    const auto hardware = hardware_.find(key);
    const bool matches = hardware != hardware_.end() && hardware->second.known &&
        std::fabs(hardware->second.value - target.value) <=
            tolerance(target.id);
    if (matches)
        pending_.erase(key);
    else
        pending_[key] = target.value;
    return wasPending != !matches ||
           (!matches && (!wasPending || oldTarget != target.value));
}

bool SoftTakeover::acceptHardware(const ControlEvent& event,
                                  bool* stateChanged)
{
    if (stateChanged) *stateChanged = false;
    if (!supports(event)) return !active();

    const Key key = keyFor(event);
    const auto pending = pending_.find(key);
    const auto previous = hardware_.find(key);
    const bool hadPrevious = previous != hardware_.end() &&
                             previous->second.known;
    const double previousValue = hadPrevious ? previous->second.value
                                             : event.value;
    hardware_[key] = HardwareValue {event.value, true};

    if (pending == pending_.end())
        return !active();

    const double target = pending->second;
    const double before = previousValue - target;
    const double after = event.value - target;
    const bool close = std::fabs(after) <= tolerance(event.id);
    const bool crossed = hadPrevious &&
        ((before < 0.0 && after > 0.0) ||
         (before > 0.0 && after < 0.0) || after == 0.0);
    if (close || crossed) {
        pending_.erase(pending);
        if (stateChanged) *stateChanged = true;
    }
    return false;
}

std::vector<SoftTakeoverState> SoftTakeover::pending() const
{
    std::vector<SoftTakeoverState> result;
    result.reserve(pending_.size());
    for (const auto& [key, target] : pending_) {
        SoftTakeoverState state;
        state.deck = key.deck;
        state.control = key.control;
        state.targetValue = target;
        const auto hardware = hardware_.find(key);
        if (hardware != hardware_.end() && hardware->second.known) {
            state.hardwareValue = hardware->second.value;
            state.hardwareKnown = true;
        }
        result.push_back(state);
    }
    return result;
}

} // namespace gvt

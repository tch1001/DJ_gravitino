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
    case ControlId::FxWet:
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
    targets_.clear();
}

void SoftTakeover::clearHardware()
{
    hardware_.clear();
    targets_.clear();
}

void SoftTakeover::rememberHardware(const ControlEvent& event)
{
    if (!supports(event)) return;
    hardware_[keyFor(event)] = HardwareValue {event.value, true};
}

void SoftTakeover::arm(const std::vector<ControlEvent>& targets)
{
    targets_.clear();
    for (const ControlEvent& target : targets) {
        if (!supports(target)) continue;
        const Key key = keyFor(target);
        const auto hardware = hardware_.find(key);
        if (hardware != hardware_.end() && hardware->second.known &&
            std::fabs(hardware->second.value - target.value) <=
                tolerance(target.id)) {
            continue;
        }
        targets_[key] = target.value;
    }
}

bool SoftTakeover::retarget(const ControlEvent& target)
{
    if (!supports(target)) return false;
    const Key key = keyFor(target);
    const auto existing = targets_.find(key);
    const bool wasPending = existing != targets_.end();
    const double oldTarget = wasPending ? existing->second : 0.0;
    targets_[key] = target.value;

    const auto hardware = hardware_.find(key);
    if (hardware != hardware_.end() && hardware->second.known &&
        std::fabs(hardware->second.value - target.value) <=
            tolerance(target.id))
        targets_.erase(key);
    return !wasPending || oldTarget != target.value || !active();
}

bool SoftTakeover::acceptHardware(const ControlEvent& event,
                                  bool* stateChanged)
{
    if (stateChanged) *stateChanged = false;
    // Pickup gates only apply to absolute controls. Buttons and other
    // relative inputs remain live while a knob is waiting for pickup.
    if (!supports(event)) return true;

    const Key key = keyFor(event);
    if (!active()) {
        hardware_[key] = HardwareValue {event.value, true};
        return true;
    }

    hardware_[key] = HardwareValue {event.value, true};
    const auto target = targets_.find(key);
    if (target == targets_.end()) return true;
    if (stateChanged) *stateChanged = true;
    if (std::fabs(event.value - target->second) <= tolerance(event.id))
        targets_.erase(target);
    // Consume the crossing event so acquiring a control never introduces a
    // small audible jump. Its next movement flows normally.
    return false;
}

std::vector<SoftTakeoverState> SoftTakeover::pending() const
{
    std::vector<SoftTakeoverState> result;
    result.reserve(targets_.size());
    for (const auto& [key, target] : targets_) {
        SoftTakeoverState state;
        state.deck = key.deck;
        state.control = key.control;
        state.targetValue = target;
        state.pickupPending = true;
        const auto hardware = hardware_.find(key);
        if (hardware != hardware_.end() && hardware->second.known) {
            state.hardwareValue = hardware->second.value;
            state.hardwareKnown = true;
            if (std::fabs(state.hardwareValue - target) <=
                tolerance(key.control))
                continue;
        }
        result.push_back(state);
    }
    return result;
}

std::vector<SoftTakeoverState> SoftTakeover::snapshot(
    const std::vector<ControlEvent>& softwareTargets) const
{
    std::vector<SoftTakeoverState> result;
    result.reserve(softwareTargets.size());
    for (const ControlEvent& target : softwareTargets) {
        if (!supports(target)) continue;
        SoftTakeoverState state;
        state.deck = target.deck;
        state.control = target.id;
        state.targetValue = target.value;
        const Key key = keyFor(target);
        const auto hardware = hardware_.find(key);
        if (hardware != hardware_.end() && hardware->second.known) {
            state.hardwareValue = hardware->second.value;
            state.hardwareKnown = true;
        }
        state.pickupPending = targets_.find(key) != targets_.end();
        result.push_back(state);
    }
    return result;
}

} // namespace gvt

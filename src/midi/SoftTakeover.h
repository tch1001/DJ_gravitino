#pragma once

#include "../control/ControlBus.h"

#include <map>
#include <vector>

namespace gvt {

// One physical absolute control that must be picked up before FLX4 input is
// re-enabled. targetValue is Gravitino's authoritative value; hardwareValue
// is the most recently observed controller position when known.
struct SoftTakeoverState {
    DeckId deck = kNoDeck;
    ControlId control = ControlId::Crossfader;
    double targetValue = 0.0;
    double hardwareValue = 0.0;
    bool hardwareKnown = false;
};

// Pure pickup/soft-takeover state machine. MidiEngine owns one instance and
// performs all mutations on the GUI thread.
class SoftTakeover {
public:
    static bool supports(const ControlEvent& event) noexcept;
    static double tolerance(ControlId control) noexcept;

    void clear();
    void clearHardware();
    void rememberHardware(const ControlEvent& event);
    void arm(const std::vector<ControlEvent>& targets);
    bool retarget(const ControlEvent& target);

    // Returns true only when the event may flow to Gravitino. While a target
    // is pending, even the pickup/crossing event is consumed so the software
    // remains exactly at its replayed value; the next physical move flows.
    bool acceptHardware(const ControlEvent& event, bool* stateChanged = nullptr);

    bool active() const noexcept { return !pending_.empty(); }
    std::vector<SoftTakeoverState> pending() const;

private:
    struct Key {
        DeckId deck = kNoDeck;
        ControlId control = ControlId::Crossfader;
        friend bool operator<(const Key& a, const Key& b) noexcept {
            if (a.deck != b.deck) return a.deck < b.deck;
            return static_cast<unsigned int>(a.control) <
                   static_cast<unsigned int>(b.control);
        }
    };
    struct HardwareValue {
        double value = 0.0;
        bool known = false;
    };

    static Key keyFor(const ControlEvent& event) noexcept {
        return Key {event.deck, event.id};
    }

    std::map<Key, HardwareValue> hardware_;
    std::map<Key, double> pending_;
};

} // namespace gvt

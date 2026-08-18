#pragma once

#include "../control/ControlBus.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace gvt {

// Stateful parser for the DDJ-FLX4's high-resolution MIDI controls. The
// controller sends the MSB first and the LSB second; continuous events are
// emitted when the LSB completes a 14-bit value.
class Flx4Mapping final {
public:
    std::optional<ControlEvent> parse(std::span<const unsigned char> message) noexcept;

    // Returns the three-byte note message used by the FLX4 for an LED, or no
    // value when the requested control has no MVP LED mapping.
    static std::optional<std::array<unsigned char, 3>> ledMessage(
        DeckId deck, ControlId id, bool on) noexcept;

    // The FLX4 reports its shared Beat FX channel assignment as two on/off
    // notes. Until the first report, both decks are selected as a safe fallback.
    std::array<bool, 2> fxAssignedDecks() const noexcept
    {
        return fxAssigned_;
    }

private:
    struct FourteenBitState {
        std::uint8_t msb = 0;
        bool hasMsb = false;
    };

    std::optional<ControlEvent> parseControlChange(
        std::uint8_t channel, std::uint8_t controller, std::uint8_t value) noexcept;
    static std::optional<ControlEvent> finishFourteenBit(
        FourteenBitState& state, bool isMsb, std::uint8_t value,
        DeckId deck, ControlId id) noexcept;

    std::array<FourteenBitState, 2> tempo_ {};
    std::array<FourteenBitState, 2> fader_ {};
    std::array<FourteenBitState, 2> trim_ {};
    std::array<FourteenBitState, 2> eqHigh_ {};
    std::array<FourteenBitState, 2> eqMid_ {};
    std::array<FourteenBitState, 2> eqLow_ {};
    std::array<FourteenBitState, 2> filter_ {};
    FourteenBitState crossfader_ {};
    FourteenBitState headphoneMix_ {};
    FourteenBitState fxWet_ {};
    std::array<bool, 2> fxAssigned_ {true, true};
    bool fxAssignmentKnown_ = false;
};

} // namespace gvt

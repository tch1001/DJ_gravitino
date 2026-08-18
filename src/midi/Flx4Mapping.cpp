#include "Flx4Mapping.h"
#include "../performance/PerformancePads.h"

#include <algorithm>
#include <cstddef>

namespace gvt {
namespace {

constexpr std::uint8_t kNoteOn = 0x90;
constexpr std::uint8_t kNoteOff = 0x80;
constexpr std::uint8_t kControlChange = 0xB0;

constexpr std::uint8_t kDeck1Channel = 0x00;
constexpr std::uint8_t kDeck2Channel = 0x01;
constexpr std::uint8_t kFxChannel1 = 0x04;
constexpr std::uint8_t kFxChannel2 = 0x05;
constexpr std::uint8_t kMixerChannel = 0x06;
constexpr std::uint8_t kDeck1HotCueChannel = 0x07;
constexpr std::uint8_t kDeck1ShiftPadChannel = 0x08;
constexpr std::uint8_t kDeck2HotCueChannel = 0x09;
constexpr std::uint8_t kDeck2ShiftPadChannel = 0x0A;

constexpr std::uint8_t kBrowseTurnController = 0x40;
constexpr std::uint8_t kBrowsePressNote = 0x41;
constexpr std::uint8_t kLoadDeck1Note = 0x46;
constexpr std::uint8_t kLoadDeck2Note = 0x47;
constexpr std::uint8_t kPlayNote = 0x0B;
constexpr std::uint8_t kCueNote = 0x0C;
constexpr std::uint8_t kHeadphoneCueNote = 0x54;
constexpr std::uint8_t kMasterCueNote = 0x63;
constexpr std::uint8_t kBeatSyncNote = 0x58;
constexpr std::uint8_t kQuantizeNote = 0x68;
constexpr std::uint8_t kHotCueModeNote = 0x1B;
constexpr std::uint8_t kPadFx1ModeNote = 0x1E;
constexpr std::uint8_t kBeatJumpModeNote = 0x20;
constexpr std::uint8_t kSamplerModeNote = 0x22;
constexpr std::uint8_t kKeyboardModeNote = 0x69;
constexpr std::uint8_t kPadFx2ModeNote = 0x6B;
constexpr std::uint8_t kBeatLoopModeNote = 0x6D;
constexpr std::uint8_t kKeyShiftModeNote = 0x6F;
constexpr std::uint8_t kLoopInNote = 0x10;
constexpr std::uint8_t kLoopOutNote = 0x11;
constexpr std::uint8_t kFourBeatExitNote = 0x4D;
constexpr std::uint8_t kShiftFourBeatExitNote = 0x50;
// The FLX4 sends the ordinary IN/OUT notes for the dual-purpose IN/1/2X and
// OUT/2X controls. The host interprets those contextually when a loop is
// active. CUE/LOOP CALL and shifted IN/OUT are useful dedicated aliases for
// loop sizing in Gravitino's MVP mapping.
constexpr std::uint8_t kCueLoopCallPreviousNote = 0x51;
constexpr std::uint8_t kCueLoopCallNextNote = 0x53;
constexpr std::uint8_t kShiftLoopInNote = 0x4C;
constexpr std::uint8_t kShiftLoopOutNote = 0x4E;
constexpr std::uint8_t kBeatFxAssignDeck1Note = 0x10;
constexpr std::uint8_t kBeatFxAssignDeck2Note = 0x11;
constexpr std::uint8_t kBeatFxOnNote = 0x47;
constexpr std::uint8_t kBeatFxLeftNote = 0x4A;
constexpr std::uint8_t kBeatFxRightNote = 0x4B;
constexpr std::uint8_t kBeatFxSelectNextNote = 0x63;
constexpr std::uint8_t kBeatFxSelectPreviousNote = 0x64;

constexpr std::uint8_t kTempoMsb = 0x00;
constexpr std::uint8_t kTempoLsb = 0x20;
constexpr std::uint8_t kFaderMsb = 0x13;
constexpr std::uint8_t kFaderLsb = 0x33;
constexpr std::uint8_t kTrimMsb = 0x04;
constexpr std::uint8_t kTrimLsb = 0x24;
constexpr std::uint8_t kEqHighMsb = 0x07;
constexpr std::uint8_t kEqHighLsb = 0x27;
constexpr std::uint8_t kEqMidMsb = 0x0B;
constexpr std::uint8_t kEqMidLsb = 0x2B;
constexpr std::uint8_t kEqLowMsb = 0x0F;
constexpr std::uint8_t kEqLowLsb = 0x2F;
constexpr std::uint8_t kCrossfaderMsb = 0x1F;
constexpr std::uint8_t kCrossfaderLsb = 0x3F;
constexpr std::uint8_t kHeadphoneMixMsb = 0x0C;
constexpr std::uint8_t kHeadphoneMixLsb = 0x2C;
constexpr std::uint8_t kFilterDeck1Msb = 0x17;
constexpr std::uint8_t kFilterDeck1Lsb = 0x37;
constexpr std::uint8_t kFilterDeck2Msb = 0x18;
constexpr std::uint8_t kFilterDeck2Lsb = 0x38;
constexpr std::uint8_t kBeatFxWetMsb = 0x02;
constexpr std::uint8_t kBeatFxWetLsb = 0x22;

constexpr std::uint8_t kJogSide = 0x21;
constexpr std::uint8_t kJogPlatterVinylOn = 0x22;
constexpr std::uint8_t kJogPlatterVinylOff = 0x23;
constexpr std::uint8_t kJogTouchNote = 0x36;
constexpr int kRelativeCenter = 0x40;

constexpr int kFourteenBitCenter = 0x2000;
constexpr int kFourteenBitMaximum = 0x3FFF;
constexpr double kTempoRange = 0.08;

bool isDeckChannel(std::uint8_t channel) noexcept
{
    return channel == kDeck1Channel || channel == kDeck2Channel;
}

DeckId deckForChannel(std::uint8_t channel) noexcept
{
    return channel == kDeck1Channel ? 0 : 1;
}

ControlId performancePadId(std::uint8_t pad) noexcept
{
    const auto first = static_cast<unsigned int>(ControlId::PerformancePad1);
    return static_cast<ControlId>(first + pad);
}

std::optional<PerformancePadMode> padModeForButton(
    std::uint8_t note) noexcept
{
    switch (note) {
    case kHotCueModeNote:  return PerformancePadMode::HotCue;
    case kPadFx1ModeNote:  return PerformancePadMode::PadFx1;
    case kBeatJumpModeNote:return PerformancePadMode::BeatJump;
    case kSamplerModeNote: return PerformancePadMode::Sampler;
    case kKeyboardModeNote:return PerformancePadMode::Keyboard;
    case kPadFx2ModeNote:  return PerformancePadMode::PadFx2;
    case kBeatLoopModeNote:return PerformancePadMode::BeatLoop;
    case kKeyShiftModeNote:return PerformancePadMode::KeyShift;
    default:               return std::nullopt;
    }
}

std::optional<PerformancePadMode> padModeForNote(
    std::uint8_t note) noexcept
{
    switch (note & 0xF0U) {
    case 0x00: return PerformancePadMode::HotCue;
    case 0x10: return PerformancePadMode::PadFx1;
    case 0x20: return PerformancePadMode::BeatJump;
    case 0x30: return PerformancePadMode::Sampler;
    case 0x40: return PerformancePadMode::Keyboard;
    case 0x50: return PerformancePadMode::PadFx2;
    case 0x60: return PerformancePadMode::BeatLoop;
    case 0x70: return PerformancePadMode::KeyShift;
    default:   return std::nullopt;
    }
}

std::uint8_t modeButtonNote(PerformancePadMode mode) noexcept
{
    switch (mode) {
    case PerformancePadMode::HotCue:  return kHotCueModeNote;
    case PerformancePadMode::PadFx1:  return kPadFx1ModeNote;
    case PerformancePadMode::BeatJump:return kBeatJumpModeNote;
    case PerformancePadMode::Sampler: return kSamplerModeNote;
    case PerformancePadMode::Keyboard:return kKeyboardModeNote;
    case PerformancePadMode::PadFx2:  return kPadFx2ModeNote;
    case PerformancePadMode::BeatLoop:return kBeatLoopModeNote;
    case PerformancePadMode::KeyShift:return kKeyShiftModeNote;
    case PerformancePadMode::SavedLoop:return 0;
    case PerformancePadMode::Count:   return 0;
    }
    return 0;
}

std::uint8_t padNoteBase(PerformancePadMode mode) noexcept
{
    switch (mode) {
    case PerformancePadMode::HotCue:  return 0x00;
    case PerformancePadMode::PadFx1:  return 0x10;
    case PerformancePadMode::BeatJump:return 0x20;
    case PerformancePadMode::Sampler: return 0x30;
    case PerformancePadMode::Keyboard:return 0x40;
    case PerformancePadMode::PadFx2:  return 0x50;
    case PerformancePadMode::BeatLoop:return 0x60;
    case PerformancePadMode::KeyShift:return 0x70;
    case PerformancePadMode::SavedLoop:return 0;
    case PerformancePadMode::Count:   return 0;
    }
    return 0;
}

bool hotCueIndex(ControlId id, std::uint8_t& index) noexcept
{
    const auto value = static_cast<unsigned int>(id);
    const auto first = static_cast<unsigned int>(ControlId::HotCue1);
    const auto last = static_cast<unsigned int>(ControlId::HotCue8);
    if (value < first || value > last) {
        return false;
    }
    index = static_cast<std::uint8_t>(value - first);
    return true;
}

double normalizedFourteenBit(int value) noexcept
{
    return static_cast<double>(value) / static_cast<double>(kFourteenBitMaximum);
}

double tempoRatio(int value) noexcept
{
    // The physical center reports 0x2000. Use two linear halves so center and
    // both +/-8% endpoints remain exact despite the asymmetric integer range.
    if (value <= kFourteenBitCenter) {
        return 1.0 - kTempoRange
            + kTempoRange * static_cast<double>(value)
                / static_cast<double>(kFourteenBitCenter);
    }

    return 1.0 + kTempoRange
        * static_cast<double>(value - kFourteenBitCenter)
        / static_cast<double>(kFourteenBitMaximum - kFourteenBitCenter);
}

} // namespace

ControlEvent Flx4Mapping::resolveLoopButtonAction(
    ControlEvent event, bool loopActive) noexcept
{
    if (!loopActive)
        return event;

    if (event.id == ControlId::LoopIn)
        event.id = ControlId::LoopHalve;
    else if (event.id == ControlId::LoopOut)
        event.id = ControlId::LoopDouble;
    return event;
}

std::optional<ControlEvent> Flx4Mapping::parse(
    std::span<const unsigned char> message) noexcept
{
    if (message.size() < 3) {
        return std::nullopt;
    }

    const auto status = static_cast<std::uint8_t>(message[0]);
    const auto command = static_cast<std::uint8_t>(status & 0xF0U);
    const auto channel = static_cast<std::uint8_t>(status & 0x0FU);
    const auto data1 = static_cast<std::uint8_t>(message[1] & 0x7FU);
    const auto data2 = static_cast<std::uint8_t>(message[2] & 0x7FU);

    if (command == kControlChange) {
        return parseControlChange(channel, data1, data2);
    }

    if (command != kNoteOn && command != kNoteOff) {
        return std::nullopt;
    }
    const bool pressed = command == kNoteOn && data2 != 0;

    if ((channel == kFxChannel1 && data1 == kBeatFxAssignDeck1Note) ||
        (channel == kFxChannel2 && data1 == kBeatFxAssignDeck2Note)) {
        if (!fxAssignmentKnown_) {
            fxAssigned_.fill(false);
            fxAssignmentKnown_ = true;
        }
        const std::size_t deck = channel == kFxChannel1 ? 0U : 1U;
        fxAssigned_[deck] = pressed;
        return std::nullopt;
    }

    if (channel == kFxChannel1 || channel == kFxChannel2) {
        switch (data1) {
        case kBeatFxOnNote:
            if (pressed)
                return ControlEvent {kNoDeck, ControlId::FxOn, 1.0};
            return std::nullopt;
        case kBeatFxLeftNote:
            if (pressed)
                return ControlEvent {kNoDeck, ControlId::FxBeats, 0.5};
            return std::nullopt;
        case kBeatFxRightNote:
            if (pressed)
                return ControlEvent {kNoDeck, ControlId::FxBeats, 2.0};
            return std::nullopt;
        case kBeatFxSelectNextNote:
            if (pressed)
                return ControlEvent {kNoDeck, ControlId::FxType, 1.0};
            return std::nullopt;
        case kBeatFxSelectPreviousNote:
            if (pressed)
                return ControlEvent {kNoDeck, ControlId::FxType, -1.0};
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    if (isDeckChannel(channel)) {
        const DeckId deck = deckForChannel(channel);
        switch (data1) {
        case kPlayNote:
            if (pressed)
                return ControlEvent {deck, ControlId::Play, 1.0};
            return std::nullopt;
        case kCueNote:
            return ControlEvent {
                deck, ControlId::Cue, pressed ? 1.0 : 0.0};
        case kHeadphoneCueNote:
            if (pressed)
                return ControlEvent {deck, ControlId::HeadphoneCue, 1.0};
            return std::nullopt;
        case kBeatSyncNote:
            if (pressed)
                return ControlEvent {deck, ControlId::TempoSync, 1.0};
            return std::nullopt;
        case kQuantizeNote:
            if (pressed)
                return ControlEvent {deck, ControlId::Quantize, 1.0};
            return std::nullopt;
        case kJogTouchNote:
            return ControlEvent {
                deck, ControlId::PlatterTouch, pressed ? 1.0 : 0.0};
        case kLoopInNote:
            if (pressed)
                return ControlEvent {deck, ControlId::LoopIn, 1.0};
            return std::nullopt;
        case kLoopOutNote:
            if (pressed)
                return ControlEvent {deck, ControlId::LoopOut, 1.0};
            return std::nullopt;
        case kFourBeatExitNote:
            if (pressed) {
                // MidiEngine changes this to LoopExit when a loop is already
                // active, matching the FLX4's combined 4 BEAT/EXIT button.
                return ControlEvent {deck, ControlId::LoopAuto, 4.0};
            }
            return std::nullopt;
        case kShiftFourBeatExitNote:
            if (pressed)
                return ControlEvent {deck, ControlId::LoopExit, 1.0};
            return std::nullopt;
        case kCueLoopCallPreviousNote:
        case kShiftLoopInNote:
            if (pressed)
                return ControlEvent {deck, ControlId::LoopHalve, 1.0};
            return std::nullopt;
        case kCueLoopCallNextNote:
        case kShiftLoopOutNote:
            if (pressed)
                return ControlEvent {deck, ControlId::LoopDouble, 1.0};
            return std::nullopt;
        default:
            if (pressed) {
                if (const auto mode = padModeForButton(data1)) {
                    return ControlEvent {
                        deck, ControlId::PerformancePadMode,
                        static_cast<double>(static_cast<int>(*mode))};
                }
            }
            return std::nullopt;
        }
    }

    if (channel == kMixerChannel && data1 == kMasterCueNote) {
        if (pressed)
            return ControlEvent {kNoDeck, ControlId::MasterCue, 1.0};
        return std::nullopt;
    }

    // Browser and LOAD controls share MIDI channel 7 (zero-based channel 6)
    // with the mixer controls, but LOAD remains deck-scoped in Gravitino.
    if (channel == kMixerChannel) {
        if (data1 == kBrowsePressNote) {
            if (pressed)
                return ControlEvent {
                    kNoDeck, ControlId::BrowseSelect, 1.0};
            return std::nullopt;
        }
        if (data1 == kLoadDeck1Note || data1 == kLoadDeck2Note) {
            if (pressed) {
                return ControlEvent {
                    data1 == kLoadDeck1Note ? 0 : 1,
                    ControlId::Load, 1.0};
            }
            return std::nullopt;
        }
    }

    const bool normalPadChannel =
        channel == kDeck1HotCueChannel || channel == kDeck2HotCueChannel;
    const bool shiftedPadChannel =
        channel == kDeck1ShiftPadChannel || channel == kDeck2ShiftPadChannel;
    if (normalPadChannel || shiftedPadChannel) {
        const DeckId deck =
            channel == kDeck1HotCueChannel ||
                    channel == kDeck1ShiftPadChannel ? 0 : 1;
        const auto mode = padModeForNote(data1);
        const std::uint8_t pad = data1 & 0x0FU;
        if (!mode || pad >= kPerformancePadCount)
            return std::nullopt;

        // The note bank describes the FLX4's private hardware latch, not
        // necessarily the layer selected in Gravitino. Preserve the reported
        // bank for validation/debugging and the SHIFT channel as an offset;
        // MidiEngine resolves the action from the host-selected layer.
        const int encodedMode = static_cast<int>(*mode) + 1 +
            (shiftedPadChannel ? kShiftedPadEncodingOffset : 0);
        return ControlEvent {
            deck, performancePadId(pad),
            pressed ? static_cast<double>(encodedMode)
                    : -static_cast<double>(encodedMode)};
    }

    return std::nullopt;
}

std::optional<ControlEvent> Flx4Mapping::parseControlChange(
    std::uint8_t channel, std::uint8_t controller, std::uint8_t value) noexcept
{
    if (channel == kMixerChannel && controller == kBrowseTurnController) {
        // The encoder reports a signed difference count: clockwise starts at
        // 0x01, counterclockwise starts at 0x7f. Zero is not movement.
        if (value == 0)
            return std::nullopt;
        const int delta = value < 0x40
            ? static_cast<int>(value)
            : static_cast<int>(value) - 0x80;
        if (delta == 0)
            return std::nullopt;
        return ControlEvent {
            kNoDeck, ControlId::BrowseNavigate,
            static_cast<double>(delta)};
    }

    if (channel == kFxChannel1) {
        if (controller == kBeatFxWetMsb) {
            return finishFourteenBit(
                fxWet_, true, value, kNoDeck, ControlId::FxWet);
        }
        if (controller == kBeatFxWetLsb) {
            return finishFourteenBit(
                fxWet_, false, value, kNoDeck, ControlId::FxWet);
        }
    }

    if (isDeckChannel(channel)) {
        const auto deckIndex = static_cast<std::size_t>(deckForChannel(channel));
        const DeckId deck = static_cast<DeckId>(deckIndex);

        switch (controller) {
        case kJogSide: {
            const int ticks = static_cast<int>(value) - kRelativeCenter;
            if (ticks == 0) {
                return std::nullopt;
            }
            return ControlEvent {deck, ControlId::Jog, static_cast<double>(ticks)};
        }
        case kJogPlatterVinylOn:
        case kJogPlatterVinylOff: {
            const int ticks = static_cast<int>(value) - kRelativeCenter;
            if (ticks == 0) {
                return std::nullopt;
            }
            return ControlEvent {
                deck, ControlId::PlatterScratch, static_cast<double>(ticks)};
        }
        case kTempoMsb:
            return finishFourteenBit(
                tempo_[deckIndex], true, value, deck, ControlId::Tempo);
        case kTempoLsb:
            return finishFourteenBit(
                tempo_[deckIndex], false, value, deck, ControlId::Tempo);
        case kFaderMsb:
            return finishFourteenBit(
                fader_[deckIndex], true, value, deck, ControlId::Fader);
        case kFaderLsb:
            return finishFourteenBit(
                fader_[deckIndex], false, value, deck, ControlId::Fader);
        case kTrimMsb:
            return finishFourteenBit(
                trim_[deckIndex], true, value, deck, ControlId::Trim);
        case kTrimLsb:
            return finishFourteenBit(
                trim_[deckIndex], false, value, deck, ControlId::Trim);
        case kEqHighMsb:
            return finishFourteenBit(
                eqHigh_[deckIndex], true, value, deck, ControlId::EqHigh);
        case kEqHighLsb:
            return finishFourteenBit(
                eqHigh_[deckIndex], false, value, deck, ControlId::EqHigh);
        case kEqMidMsb:
            return finishFourteenBit(
                eqMid_[deckIndex], true, value, deck, ControlId::EqMid);
        case kEqMidLsb:
            return finishFourteenBit(
                eqMid_[deckIndex], false, value, deck, ControlId::EqMid);
        case kEqLowMsb:
            return finishFourteenBit(
                eqLow_[deckIndex], true, value, deck, ControlId::EqLow);
        case kEqLowLsb:
            return finishFourteenBit(
                eqLow_[deckIndex], false, value, deck, ControlId::EqLow);
        default:
            return std::nullopt;
        }
    }

    if (channel == kMixerChannel) {
        if (controller == kHeadphoneMixMsb) {
            return finishFourteenBit(
                headphoneMix_, true, value, kNoDeck,
                ControlId::HeadphoneMix);
        }
        if (controller == kHeadphoneMixLsb) {
            return finishFourteenBit(
                headphoneMix_, false, value, kNoDeck,
                ControlId::HeadphoneMix);
        }
        if (controller == kFilterDeck1Msb) {
            return finishFourteenBit(
                filter_[0], true, value, 0, ControlId::Filter);
        }
        if (controller == kFilterDeck1Lsb) {
            return finishFourteenBit(
                filter_[0], false, value, 0, ControlId::Filter);
        }
        if (controller == kFilterDeck2Msb) {
            return finishFourteenBit(
                filter_[1], true, value, 1, ControlId::Filter);
        }
        if (controller == kFilterDeck2Lsb) {
            return finishFourteenBit(
                filter_[1], false, value, 1, ControlId::Filter);
        }
        if (controller == kCrossfaderMsb) {
            return finishFourteenBit(
                crossfader_, true, value, kNoDeck, ControlId::Crossfader);
        }
        if (controller == kCrossfaderLsb) {
            return finishFourteenBit(
                crossfader_, false, value, kNoDeck, ControlId::Crossfader);
        }
    }

    return std::nullopt;
}

std::optional<ControlEvent> Flx4Mapping::finishFourteenBit(
    FourteenBitState& state, bool isMsb, std::uint8_t value,
    DeckId deck, ControlId id) noexcept
{
    if (isMsb) {
        state.msb = value;
        state.hasMsb = true;
        return std::nullopt;
    }
    if (!state.hasMsb) {
        return std::nullopt;
    }

    const int fullValue = (static_cast<int>(state.msb) << 7)
        | static_cast<int>(value);
    const double mappedValue = id == ControlId::Tempo
        ? tempoRatio(fullValue)
        : std::clamp(normalizedFourteenBit(fullValue), 0.0, 1.0);
    return ControlEvent {deck, id, mappedValue};
}

std::optional<std::array<unsigned char, 3>> Flx4Mapping::ledMessage(
    DeckId deck, ControlId id, bool on) noexcept
{
    if (deck < 0 || deck > 1) {
        return std::nullopt;
    }

    std::uint8_t status = static_cast<std::uint8_t>(kNoteOn + deck);
    std::uint8_t note = 0;

    if (id == ControlId::Play) {
        note = kPlayNote;
    } else if (id == ControlId::Cue) {
        note = kCueNote;
    } else if (id == ControlId::HeadphoneCue) {
        note = kHeadphoneCueNote;
    } else if (id == ControlId::Quantize) {
        note = kQuantizeNote;
    } else if (id == ControlId::LoopIn) {
        note = kLoopInNote;
    } else if (id == ControlId::LoopOut) {
        note = kLoopOutNote;
    } else if (id == ControlId::LoopExit) {
        note = kFourBeatExitNote;
    } else if (id == ControlId::LoopHalve) {
        note = kShiftLoopInNote;
    } else if (id == ControlId::LoopDouble) {
        note = kShiftLoopOutNote;
    } else if (id == ControlId::LoopAuto) {
        note = kShiftFourBeatExitNote;
    } else if (id == ControlId::FxOn) {
        status = deck == 0
            ? static_cast<std::uint8_t>(kNoteOn + kFxChannel1)
            : static_cast<std::uint8_t>(kNoteOn + kFxChannel2);
        note = kBeatFxOnNote;
    } else {
        std::uint8_t pad = 0;
        if (!hotCueIndex(id, pad)) {
            return std::nullopt;
        }
        status = deck == 0 ? 0x97 : 0x99;
        note = pad;
    }

    return std::array<unsigned char, 3> {
        status, note, static_cast<unsigned char>(on ? 0x7F : 0x00)};
}

std::optional<std::array<unsigned char, 3>>
Flx4Mapping::padModeLedMessage(DeckId deck, int mode, bool on) noexcept
{
    if (deck < 0 || deck > 1 || mode < 0 ||
        mode >= static_cast<int>(PerformancePadMode::SavedLoop)) {
        return std::nullopt;
    }
    const auto typedMode = static_cast<PerformancePadMode>(mode);
    return std::array<unsigned char, 3> {
        static_cast<unsigned char>(kNoteOn + deck),
        modeButtonNote(typedMode),
        static_cast<unsigned char>(on ? 0x7F : 0x00)};
}

std::optional<std::array<unsigned char, 3>>
Flx4Mapping::performancePadLedMessage(
    DeckId deck, int mode, int pad, bool shifted,
    unsigned char velocity) noexcept
{
    if (deck < 0 || deck > 1 || mode < 0 ||
        mode >= static_cast<int>(PerformancePadMode::SavedLoop) ||
        pad < 0 || pad >= kPerformancePadCount) {
        return std::nullopt;
    }

    const auto typedMode = static_cast<PerformancePadMode>(mode);
    const unsigned char status = static_cast<unsigned char>(
        shifted ? (deck == 0 ? 0x98 : 0x9A)
                : (deck == 0 ? 0x97 : 0x99));
    return std::array<unsigned char, 3> {
        status,
        static_cast<unsigned char>(padNoteBase(typedMode) + pad),
        static_cast<unsigned char>(velocity & 0x7FU)};
}

} // namespace gvt

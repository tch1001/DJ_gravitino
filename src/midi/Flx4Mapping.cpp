#include "Flx4Mapping.h"

#include <algorithm>
#include <cstddef>

namespace gvt {
namespace {

constexpr std::uint8_t kNoteOn = 0x90;
constexpr std::uint8_t kNoteOff = 0x80;
constexpr std::uint8_t kControlChange = 0xB0;

constexpr std::uint8_t kDeck1Channel = 0x00;
constexpr std::uint8_t kDeck2Channel = 0x01;
constexpr std::uint8_t kMixerChannel = 0x06;
constexpr std::uint8_t kDeck1HotCueChannel = 0x07;
constexpr std::uint8_t kDeck2HotCueChannel = 0x09;

constexpr std::uint8_t kPlayNote = 0x0B;
constexpr std::uint8_t kCueNote = 0x0C;
constexpr std::uint8_t kBeatSyncNote = 0x58;

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

constexpr std::uint8_t kJogSide = 0x21;
constexpr std::uint8_t kJogPlatterVinylOn = 0x22;
constexpr std::uint8_t kJogPlatterVinylOff = 0x23;
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

ControlId hotCueId(std::uint8_t pad) noexcept
{
    const auto first = static_cast<unsigned int>(ControlId::HotCue1);
    return static_cast<ControlId>(first + pad);
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
        case kBeatSyncNote:
            if (pressed)
                return ControlEvent {deck, ControlId::TempoSync, 1.0};
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    if ((channel == kDeck1HotCueChannel || channel == kDeck2HotCueChannel)
        && data1 < 8) {
        const DeckId deck = channel == kDeck1HotCueChannel ? 0 : 1;
        return ControlEvent {
            deck, hotCueId(data1), pressed ? 1.0 : 0.0};
    }

    return std::nullopt;
}

std::optional<ControlEvent> Flx4Mapping::parseControlChange(
    std::uint8_t channel, std::uint8_t controller, std::uint8_t value) noexcept
{
    if (isDeckChannel(channel)) {
        const auto deckIndex = static_cast<std::size_t>(deckForChannel(channel));
        const DeckId deck = static_cast<DeckId>(deckIndex);

        switch (controller) {
        case kJogSide:
        case kJogPlatterVinylOn:
        case kJogPlatterVinylOff: {
            const int ticks = static_cast<int>(value) - kRelativeCenter;
            if (ticks == 0) {
                return std::nullopt;
            }
            return ControlEvent {deck, ControlId::Jog, static_cast<double>(ticks)};
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

} // namespace gvt

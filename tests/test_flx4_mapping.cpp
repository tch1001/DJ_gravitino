#include "control/ControlBus.h"
#include "midi/Flx4Mapping.h"

#include <array>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

void checkEvent(const std::optional<gvt::ControlEvent>& event,
                gvt::DeckId deck, gvt::ControlId id)
{
    CHECK(event.has_value());
    if (!event)
        return;
    CHECK(event->deck == deck);
    CHECK(event->id == id);
    CHECK(event->value == 1.0);
}
}

int main()
{
    using namespace gvt;

    Flx4Mapping mapping;

    // The five-segment channel meters are host-driven CC messages, not notes.
    const auto meterA = Flx4Mapping::channelLevelMessage(0, 0x57);
    const auto meterB = Flx4Mapping::channelLevelMessage(1, 0x7F);
    CHECK(meterA && (*meterA ==
        std::array<unsigned char, 3> {0xB0, 0x02, 0x57}));
    CHECK(meterB && (*meterB ==
        std::array<unsigned char, 3> {0xB1, 0x02, 0x7F}));
    CHECK(!Flx4Mapping::channelLevelMessage(kNoDeck, 0x40));

    // The dual-purpose physical IN/1/2X and OUT/2X controls always report
    // their ordinary IN/OUT notes. MidiEngine decides whether these mean
    // LoopIn/LoopOut or LoopHalve/LoopDouble from the deck's active-loop state.
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x90, 0x10, 0x7F}),
               0, ControlId::LoopIn);
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x91, 0x10, 0x7F}),
               1, ControlId::LoopIn);
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x90, 0x11, 0x7F}),
               0, ControlId::LoopOut);
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x91, 0x11, 0x7F}),
               1, ControlId::LoopOut);

    // Once a loop is active, those same physical messages select the second
    // label printed on each control instead of replacing the manual bounds.
    auto resolved = Flx4Mapping::resolveLoopButtonAction(
        ControlEvent {0, ControlId::LoopIn, 1.0}, true);
    CHECK(resolved.id == ControlId::LoopHalve);
    resolved = Flx4Mapping::resolveLoopButtonAction(
        ControlEvent {1, ControlId::LoopOut, 1.0}, true);
    CHECK(resolved.id == ControlId::LoopDouble);
    resolved = Flx4Mapping::resolveLoopButtonAction(
        ControlEvent {0, ControlId::LoopIn, 1.0}, false);
    CHECK(resolved.id == ControlId::LoopIn);

    // Button releases must not retrigger a loop operation.
    CHECK(!mapping.parse(
        std::array<unsigned char, 3> {0x90, 0x10, 0x00}));
    CHECK(!mapping.parse(
        std::array<unsigned char, 3> {0x91, 0x11, 0x00}));

    // Gravitino also exposes loop sizing through CUE/LOOP CALL and the shifted
    // IN/OUT messages documented by the FLX4 MIDI message list.
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x90, 0x51, 0x7F}),
               0, ControlId::LoopHalve);
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x91, 0x53, 0x7F}),
               1, ControlId::LoopDouble);
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x90, 0x4C, 0x7F}),
               0, ControlId::LoopHalve);
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x91, 0x4E, 0x7F}),
               1, ControlId::LoopDouble);

    // The center browser lives on MIDI channel 7. Rotation is a signed
    // difference count; pressing the encoder confirms the visible row.
    auto event = mapping.parse(
        std::array<unsigned char, 3> {0xB6, 0x40, 0x01});
    CHECK(event.has_value());
    if (event) {
        CHECK(event->deck == kNoDeck);
        CHECK(event->id == ControlId::BrowseNavigate);
        CHECK(event->value == 1.0);
    }
    event = mapping.parse(
        std::array<unsigned char, 3> {0xB6, 0x40, 0x7F});
    CHECK(event.has_value());
    if (event) {
        CHECK(event->deck == kNoDeck);
        CHECK(event->id == ControlId::BrowseNavigate);
        CHECK(event->value == -1.0);
    }
    CHECK(!mapping.parse(
        std::array<unsigned char, 3> {0xB6, 0x40, 0x00}));
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x96, 0x41, 0x7F}),
               kNoDeck, ControlId::BrowseSelect);
    CHECK(!mapping.parse(
        std::array<unsigned char, 3> {0x96, 0x41, 0x00}));

    // LOAD buttons are global-channel notes but target their printed deck.
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x96, 0x46, 0x7F}),
               0, ControlId::Load);
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x96, 0x47, 0x7F}),
               1, ControlId::Load);
    CHECK(!mapping.parse(
        std::array<unsigned char, 3> {0x96, 0x46, 0x00}));

    // Physical BEAT SYNC is the app's one-shot, phase-only sync action.
    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x90, 0x58, 0x7F}),
               0, ControlId::TempoSync);

    // The FLX4 reports SHIFT + BEAT SYNC as its dedicated tempo-range note.
    // The host cycles the deck through Serato's 8/16/50% choices.
    event = mapping.parse(
        std::array<unsigned char, 3> {0x91, 0x60, 0x7F});
    CHECK(event.has_value());
    if (event) {
        CHECK(event->deck == 1);
        CHECK(event->id == ControlId::TempoRange);
        CHECK(event->value == 0.0);
    }
    CHECK(!mapping.parse(
        std::array<unsigned char, 3> {0x91, 0x60, 0x00}));

    // The jog rim remains a fine transient tempo nudge. Top/platter rotation
    // is a separate coarse position scrub in either vinyl mode.
    event = mapping.parse(
        std::array<unsigned char, 3> {0xB0, 0x21, 0x41});
    CHECK(event.has_value());
    if (event) {
        CHECK(event->deck == 0);
        CHECK(event->id == ControlId::Jog);
        CHECK(event->value == 1.0);
    }
    event = mapping.parse(
        std::array<unsigned char, 3> {0xB1, 0x22, 0x3E});
    CHECK(event.has_value());
    if (event) {
        CHECK(event->deck == 1);
        CHECK(event->id == ControlId::PlatterScratch);
        CHECK(event->value == -2.0);
    }
    event = mapping.parse(
        std::array<unsigned char, 3> {0xB0, 0x23, 0x43});
    CHECK(event.has_value());
    if (event) {
        CHECK(event->deck == 0);
        CHECK(event->id == ControlId::PlatterScratch);
        CHECK(event->value == 3.0);
    }
    CHECK(!mapping.parse(
        std::array<unsigned char, 3> {0xB0, 0x22, 0x40}));

    // Capacitive top contact brackets audible scratching. Both press and
    // release are retained so ordinary transport can be suspended/restored.
    event = mapping.parse(
        std::array<unsigned char, 3> {0x90, 0x36, 0x7F});
    CHECK(event && event->deck == 0 &&
          event->id == ControlId::PlatterTouch && event->value == 1.0);
    event = mapping.parse(
        std::array<unsigned char, 3> {0x91, 0x36, 0x00});
    CHECK(event && event->deck == 1 &&
          event->id == ControlId::PlatterTouch && event->value == 0.0);

    // Pad-mode buttons select the corresponding virtual layer. Pad notes also
    // report the FLX4's private hardware bank, but MidiEngine deliberately
    // resolves their action from Gravitino's selected layer.
    event = mapping.parse(
        std::array<unsigned char, 3> {0x90, 0x1E, 0x7F});
    CHECK(event && event->deck == 0 &&
          event->id == ControlId::PerformancePadMode &&
          event->value == 1.0); // PadFx1 enum value
    event = mapping.parse(
        std::array<unsigned char, 3> {0x99, 0x23, 0x7F});
    CHECK(event && event->deck == 1 &&
          event->id == ControlId::PerformancePad4 &&
          event->value == 3.0); // BeatJump enum value + 1
    event = mapping.parse(
        std::array<unsigned char, 3> {0x99, 0x23, 0x00});
    CHECK(event && event->id == ControlId::PerformancePad4 &&
          event->value == -3.0);
    event = mapping.parse(
        std::array<unsigned char, 3> {0x97, 0x65, 0x7F});
    CHECK(event && event->id == ControlId::PerformancePad6 &&
          event->value == 7.0); // BeatLoop enum value + 1

    // SHIFT is preserved independently of the controller-reported bank. The
    // host decides whether it means HOT CUE delete from its selected layer.
    event = mapping.parse(
        std::array<unsigned char, 3> {0x98, 0x02, 0x7F});
    CHECK(event && event->deck == 0 &&
          event->id == ControlId::PerformancePad3 &&
          event->value == 101.0);
    event = mapping.parse(
        std::array<unsigned char, 3> {0x98, 0x02, 0x00});
    CHECK(event && event->id == ControlId::PerformancePad3 &&
          event->value == -101.0);

    // Even normal HOT CUE-bank packets use the generic pad path now, allowing
    // a virtual PAD FX/BEAT JUMP selection to override the hardware latch.
    event = mapping.parse(
        std::array<unsigned char, 3> {0x97, 0x04, 0x7F});
    CHECK(event && event->id == ControlId::PerformancePad5 &&
          event->value == 1.0);
    event = mapping.parse(
        std::array<unsigned char, 3> {0x97, 0x04, 0x00});
    CHECK(event && event->id == ControlId::PerformancePad5 &&
          event->value == -1.0);

    checkEvent(mapping.parse(
                   std::array<unsigned char, 3> {0x91, 0x68, 0x7F}),
               1, ControlId::Quantize);

    const auto modeLed = Flx4Mapping::padModeLedMessage(1, 6, true);
    CHECK(modeLed && (*modeLed)[0] == 0x91 && (*modeLed)[1] == 0x6D &&
          (*modeLed)[2] == 0x7F);
    const auto padLed = Flx4Mapping::performancePadLedMessage(
        0, 3, 4, true, 0x30);
    CHECK(padLed && (*padLed)[0] == 0x98 && (*padLed)[1] == 0x34 &&
          (*padLed)[2] == 0x30);

    if (failures)
        return 1;
    std::printf("test_flx4_mapping: FLX4 loop-note mapping passed\n");
    return 0;
}

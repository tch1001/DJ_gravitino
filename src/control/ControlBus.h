// PINNED INTERFACE — see docs/ARCHITECTURE.md before changing.
#pragma once
#include <QObject>
#include <cstdint>

namespace gvt {

// Deck roles. Physical deck index for the MVP: 0 = left/A, 1 = right/B.
// -1 = not deck-scoped (mixer/global controls).
using DeckId = int;
constexpr DeckId kNoDeck = -1;

enum class ControlId : uint8_t {
    // Triggers (value: 1.0 = press; 0.0 = release where relevant)
    Play, Stop, Cue, Load, TempoSync,
    HotCue1, HotCue2, HotCue3, HotCue4,
    HotCue5, HotCue6, HotCue7, HotCue8,
    // Continuous, normalized 0..1 unless noted
    Tempo,        // playback ratio, 1.0 = native speed (range ~0.84..1.16)
    Fader,        // channel fader
    Trim,         // gain knob
    EqLow, EqMid, EqHigh,   // 0.5 = flat, 0.0 = kill
    Crossfader,   // 0 = full deck A ... 1 = full deck B (deck = kNoDeck)
    Jog,          // signed nudge ticks (deck-scoped, transient)
    Count
};

bool controlIsTrigger(ControlId id);
// Stable wire names used by .gvt files and debugging ("play", "eq_low", ...).
const char* controlName(ControlId id);
bool controlFromName(const char* name, ControlId& out); // false = unknown

struct ControlEvent {
    DeckId    deck  = kNoDeck;
    ControlId id    = ControlId::Play;
    double    value = 1.0;
};

// Origin of an event, so consumers can avoid feedback loops
// (e.g. the MIDI LED writer ignores Origin::Midi; the transition recorder
// records Human origins only, not Replay).
enum class Origin : uint8_t { Ui, Midi, Replay, System };

// Single dispatch point for all control actions. GUI-thread only.
class ControlBus : public QObject {
    Q_OBJECT
public:
    explicit ControlBus(QObject* parent = nullptr);
    // Emit an event onto the bus (synchronously fires eventDispatched).
    void dispatch(const ControlEvent& e, Origin origin);
signals:
    void eventDispatched(const gvt::ControlEvent& e, gvt::Origin origin);
};

} // namespace gvt

Q_DECLARE_METATYPE(gvt::ControlEvent)
Q_DECLARE_METATYPE(gvt::Origin)

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
    Play, Stop, Cue, Load, TempoSync, // one-shot phase align; no tempo change
    HotCue1, HotCue2, HotCue3, HotCue4,
    HotCue5, HotCue6, HotCue7, HotCue8,
    LoopIn, LoopOut,   // whole-beat snapped only while Quantize is enabled
    LoopExit,          // exit active loop (RELOOP re-enters via LoopIn history)
    LoopHalve, LoopDouble,
    // Continuous, normalized 0..1 unless noted
    Tempo,        // playback ratio, 1.0 = native (selected range up to ±50%)
    Fader,        // channel fader
    Trim,         // gain knob
    EqLow, EqMid, EqHigh,   // 0.5 = flat, 0.0 = kill
    Crossfader,   // 0 = full deck A ... 1 = full deck B (deck = kNoDeck)
    HeadphoneCue, // per-deck PFL state, 0 = off / 1 = on
    MasterCue,    // master-to-headphones state (deck = kNoDeck)
    HeadphoneMix, // 0 = CUE ... 1 = MASTER (deck = kNoDeck)
    Jog,          // signed nudge ticks (deck-scoped, transient)
    LoopAuto,     // value = loop length in beats (e.g. 4.0); starts beat-snapped loop
    BeatJump,     // value = signed beats to jump (e.g. -8, 4), beat-aligned
    Filter,       // per-deck DJ filter: 0.5 = off, <0.5 low-pass, >0.5 high-pass
    FxType,       // per-deck FX select: 0 = echo, 1 = reverb, 2 = flanger
    FxOn,         // trigger: 1.0 = engage, 0.0 = disengage (state, not toggle)
    FxWet,        // 0..1 dry/wet
    FxBeats,      // echo/flanger time base in beats (0.25..4, default 0.5)
    // Stem levels, 0..1 (0 = muted). Active only once stems are separated.
    StemVocals, StemMelody, StemBass, StemDrums,
    // Hardware-library UI commands (kept at the end to preserve existing
    // ControlId numeric values; MidiEngine does not publish these to audio).
    BrowseSelect,
    BrowseNavigate, // signed library-row delta from the browse encoder
    PlatterScratch, // signed coarse position-scrub ticks (deck-scoped)
    Quantize,       // per-deck hot-cue/manual-loop quantize state
    PerformancePadMode, // value = PerformancePadMode enum value
    PerformancePad1, PerformancePad2, PerformancePad3, PerformancePad4,
    PerformancePad5, PerformancePad6, PerformancePad7, PerformancePad8,
    PlatterTouch,   // top-platter contact state, 1 = held / 0 = released
    // Per-track CUSTOM/saved-loop pads. These are release-aware like hot cues:
    // hold previews the stored loop; PLAY while held latches transport.
    SavedLoop1, SavedLoop2, SavedLoop3, SavedLoop4,
    SavedLoop5, SavedLoop6, SavedLoop7, SavedLoop8,
    TempoRange, // per-deck selected fader range; 0 cycles 8% -> 16% -> 50%
    // Transition-owned temporary CUSTOM cues. Unlike HotCue*, these never
    // read or write TrackData::hotCues.
    TransitionCue1, TransitionCue2, TransitionCue3, TransitionCue4,
    TransitionCue5, TransitionCue6, TransitionCue7, TransitionCue8,
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

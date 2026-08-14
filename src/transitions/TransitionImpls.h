// INTERNAL header (not pinned) — full Impl definitions for TransitionRecorder
// and TransitionPlayer. They live in one header because TransitionEngine.h has
// no same-named .cpp: its moc output is included from TransitionRecorder.cpp,
// and the moc TU implicitly defines the (virtual) destructors, which requires
// BOTH Impl types to be complete there. Owner: claude-transitions.
#pragma once
#include <QElapsedTimer>
#include <QTimer>
#include <vector>
#include "TransitionEngine.h"
#include "PlayerMath.h"

namespace gvt {

struct TransitionRecorder::Impl {
    ControlBus*  bus = nullptr;
    AudioEngine* engine = nullptr;

    bool   recording = false;
    int    fromDeck = 0;
    int    toDeck = 1;
    double anchorBeat = 0.0;
    double masterBpm = 0.0;
    bool   toAnchorSet = false;
    double toAnchorBeat = 0.0;

    // Wall-clock extrapolation if the from-deck stops mid-recording.
    bool          haveLastBeat = false;
    double        lastBeat = 0.0;
    QElapsedTimer sinceLastBeat;

    std::vector<GvtEvent> events;

    // Beats since anchor; keeps counting on wall time if the deck stops.
    double currentBeat() {
        Deck& d = engine->deck(fromDeck);
        if (d.playing.load()) {
            const double b = d.beatPosition() - anchorBeat;
            lastBeat = b;
            haveLastBeat = true;
            sinceLastBeat.restart();
            return b;
        }
        if (haveLastBeat && masterBpm > 0.0)
            return lastBeat +
                   sinceLastBeat.nsecsElapsed() * 1e-9 * masterBpm / 60.0;
        return d.beatPosition() - anchorBeat;
    }
};

struct TransitionPlayer::Impl {
    ControlBus*  bus = nullptr;
    AudioEngine* engine = nullptr;
    QTimer       timer;

    bool       active = false;
    PlayerMode mode = PlayerMode::Perform;
    GvtFile    file;
    int        fromDeck = 0, toDeck = 1;
    double     anchorFrom = 0.0;
    double     totalBeats = 0.0;

    std::vector<ScheduledEvent> sched;
    std::vector<uint8_t> done;      // fired (Perform) / resolved (Tutorial)
    std::vector<uint8_t> prompted;  // Tutorial: prompt emitted

    // Wall-clock extrapolation if the from-deck stops mid-transition (most
    // transitions end with a FromDeck stop; without this the beat clock
    // freezes and finished() never fires).
    bool          haveLastBeat = false;
    double        lastBeat = 0.0;
    QElapsedTimer sinceLastBeat;

    DeckId physicalDeck(Role r) const {
        switch (r) {
            case Role::FromDeck: return fromDeck;
            case Role::ToDeck:   return toDeck;
            case Role::Mixer:    return kNoDeck;
        }
        return kNoDeck;
    }

    // The .gvt crossfader is stored in ROLE space: 0 = from-deck, 1 = to-deck.
    // Physically 0 = deck A, so mirror when the from role sits on deck B.
    double xfaderRoleToPhysical(double v) const { return fromDeck == 0 ? v : 1.0 - v; }

    double currentRel() {
        Deck& d = engine->deck(fromDeck);
        if (d.playing.load()) {
            const double b = d.beatPosition() - anchorFrom;
            lastBeat = b;
            haveLastBeat = true;
            sinceLastBeat.restart();
            return b;
        }
        const double bpm = file.masterBpm > 0.0 ? file.masterBpm : d.effectiveBpm();
        if (haveLastBeat && bpm > 0.0)
            return lastBeat + sinceLastBeat.nsecsElapsed() * 1e-9 * bpm / 60.0;
        return d.beatPosition() - anchorFrom;
    }

    // Engine's current value for a (role, control) — glide start fallback.
    double engineValue(Role role, ControlId id) const {
        if (role == Role::Mixer)
            return (id == ControlId::Crossfader)
                       ? xfaderRoleToPhysical(engine->crossfader.load()) // to role space (self-inverse)
                       : 0.0;
        const Deck& d = engine->deck(physicalDeck(role));
        switch (id) {
            case ControlId::Tempo:  return d.tempoRatio.load();
            case ControlId::Fader:  return d.fader.load();
            case ControlId::Trim:   return d.trim.load();
            case ControlId::EqLow:  return d.eqLow.load();
            case ControlId::EqMid:  return d.eqMid.load();
            case ControlId::EqHigh: return d.eqHigh.load();
            default:                return 0.0;
        }
    }

    void dispatch(Role role, ControlId id, double value) {
        ControlEvent e;
        e.deck = physicalDeck(role);
        e.id = id;
        e.value = (id == ControlId::Crossfader) ? xfaderRoleToPhysical(value) : value;
        bus->dispatch(e, Origin::Replay);
    }

    void fireFinal(const ScheduledEvent& s) {
        // Special case: a ToDeck 'play' trigger seeks the incoming track to
        // its anchor beat first, so replay aligns even if the deck was never
        // cued there.
        if (s.e.role == Role::ToDeck && s.e.control == ControlId::Play &&
            s.e.value >= 0.5 && !engine->deck(toDeck).playing.load()) {
            if (TrackDataPtr t = engine->deck(toDeck).track())
                engine->deck(toDeck).seekSec(t->secAtBeat(file.anchorToBeat));
        }
        dispatch(s.e.role, s.e.control, s.e.value);
    }
};

} // namespace gvt

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
    GvtInitialState initialFrom;
    GvtInitialState initialTo;
    std::array<double, 8> fromHotCueBeats {
        kUnmappedHotCueBeat, kUnmappedHotCueBeat, kUnmappedHotCueBeat,
        kUnmappedHotCueBeat, kUnmappedHotCueBeat, kUnmappedHotCueBeat,
        kUnmappedHotCueBeat, kUnmappedHotCueBeat};
    std::array<double, 8> toHotCueBeats {
        kUnmappedHotCueBeat, kUnmappedHotCueBeat, kUnmappedHotCueBeat,
        kUnmappedHotCueBeat, kUnmappedHotCueBeat, kUnmappedHotCueBeat,
        kUnmappedHotCueBeat, kUnmappedHotCueBeat};
    double initialCrossfader = 0.0; // role space
    bool   toAnchorSet = false;
    double toAnchorBeat = 0.0;

    // Monotonic musical clock. Track beatPosition() wraps backwards whenever
    // an outgoing loop repeats, so it cannot be the transition timeline.
    bool          haveLastBeat = false;
    double        lastBeat = 0.0;
    bool          timelineRunning = false;
    double        timelineBpm = 0.0;
    double        wallBeat = 0.0;
    double        deckBeat = 0.0;
    double        lastDeckTrackBeat = 0.0;
    bool          haveDeckTrackBeat = false;
    QElapsedTimer timelineClock;
    qint64        lastTimelineNs = 0;

    std::vector<GvtEvent> events;
    bool gesturePending = false;
    DeckId gestureDeck = kNoDeck;
    ControlId gestureControl = ControlId::Count;
    int gesturePadMode = -1;

    // Beats since Record began. It follows the outgoing deck's effective BPM,
    // continues after an end-of-transition Stop, and never rewinds on loops.
    double currentBeat(const ControlEvent* cause = nullptr) {
        Deck& d = engine->deck(fromDeck);
        const qint64 now = timelineClock.isValid()
                               ? timelineClock.nsecsElapsed() : 0;
        if (timelineRunning && timelineBpm > 0.0 && now >= lastTimelineNs)
            wallBeat += static_cast<double>(now - lastTimelineNs) * 1e-9 *
                        timelineBpm / 60.0;
        lastTimelineNs = now;

        const double currentTrackBeat = d.beatPosition();
        const bool causedSeek = cause && cause->deck == fromDeck &&
            (cause->id == ControlId::Cue ||
             cause->id == ControlId::TempoSync ||
             (cause->id >= ControlId::HotCue1 &&
              cause->id <= ControlId::HotCue8) ||
             (cause->id >= ControlId::SavedLoop1 &&
              cause->id <= ControlId::SavedLoop8) ||
             cause->id == ControlId::BeatJump ||
             cause->id == ControlId::PlatterScratch);
        if (haveDeckTrackBeat && !causedSeek) {
            const double delta = currentTrackBeat - lastDeckTrackBeat;
            if (delta >= 0.0) {
                deckBeat += delta;
            } else if (d.loopActive.load()) {
                const TrackDataPtr track = d.track();
                if (track) {
                    const double loopStart =
                        track->beatAtSec(d.loopStartSec.load());
                    const double loopEnd =
                        track->beatAtSec(d.loopEndSec.load());
                    const double wrapped =
                        (loopEnd - lastDeckTrackBeat) +
                        (currentTrackBeat - loopStart);
                    if (loopEnd > loopStart && wrapped >= 0.0 &&
                        lastDeckTrackBeat >= loopStart - 0.05 &&
                        lastDeckTrackBeat <= loopEnd + 0.05 &&
                        currentTrackBeat >= loopStart - 0.05 &&
                        currentTrackBeat <= loopEnd + 0.05)
                        deckBeat += wrapped;
                }
            }
        }
        lastDeckTrackBeat = currentTrackBeat;
        haveDeckTrackBeat = true;

        if (d.playing.load()) {
            haveLastBeat = true;
            timelineRunning = true;
        } else if (haveLastBeat) {
            // Continue the transition after the outgoing STOP from the latest
            // authoritative deck time, not a slower wall-time shadow.
            wallBeat = std::max(wallBeat, deckBeat);
        }
        // AudioEngine receives the bus event before Recorder, so for a Tempo
        // event the elapsed interval above used the old rate and this stores
        // the new rate for the next interval.
        const double effective = d.effectiveBpm();
        if (effective > 0.0) timelineBpm = effective;
        lastBeat = std::max({lastBeat, wallBeat, deckBeat});
        return haveLastBeat ? lastBeat : 0.0;
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
    bool       preStateTransportApplied = false;
    ControlId  incomingPreviewControl = ControlId::Count;

    std::vector<ScheduledEvent> sched;
    std::vector<uint8_t> done;      // fired (Perform) / resolved (Tutorial)
    std::vector<uint8_t> prompted;  // Tutorial: prompt emitted

    // Monotonic musical clock. Once the anchor is reached it follows the
    // outgoing deck's effective BPM without reading its wrapping loop
    // position, and continues after the outgoing track is stopped.
    bool          haveLastBeat = false;
    double        lastBeat = 0.0;
    bool          timelineStarted = false;
    bool          timelineRunning = false;
    double        timelineBpm = 0.0;
    double        wallBeat = 0.0;
    double        deckBeat = 0.0;
    double        lastDeckTrackBeat = 0.0;
    bool          haveDeckTrackBeat = false;
    QElapsedTimer timelineClock;
    qint64        lastTimelineNs = 0;

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
        const auto deckTrackBeat = [this, &d] {
            const TrackDataPtr track = d.track();
            return track ? transitionBeatAtSec(file, *track,
                                                d.positionSec())
                         : d.beatPosition();
        };
        if (!timelineStarted) {
            const double raw = deckTrackBeat() - anchorFrom;
            if (raw < 0.0) return raw;
            timelineStarted = true;
            timelineRunning = d.playing.load();
            haveLastBeat = timelineRunning;
            lastBeat = raw;
            wallBeat = raw;
            deckBeat = raw;
            lastDeckTrackBeat = deckTrackBeat();
            haveDeckTrackBeat = true;
            timelineBpm = d.effectiveBpm() > 0.0
                              ? d.effectiveBpm() : file.masterBpm;
            timelineClock.start();
            lastTimelineNs = 0;
            return lastBeat;
        }

        const qint64 now = timelineClock.nsecsElapsed();
        const double elapsedBeats =
            timelineRunning && timelineBpm > 0.0 && now >= lastTimelineNs
                ? static_cast<double>(now - lastTimelineNs) * 1e-9 *
                      timelineBpm / 60.0
                : 0.0;
        if (timelineRunning && timelineBpm > 0.0 && now >= lastTimelineNs)
            wallBeat += elapsedBeats;
        lastTimelineNs = now;
        if (d.playing.load()) {
            if (!timelineRunning) wallBeat = lastBeat;
            haveLastBeat = true;
            timelineRunning = true;
            const double currentTrackBeat = deckTrackBeat();
            if (haveDeckTrackBeat) {
                const double delta = currentTrackBeat - lastDeckTrackBeat;
                double forward = 0.0;
                if (delta >= 0.0) {
                    // A positive deck delta is authoritative. This also lets
                    // deterministic/offline rendering advance much faster
                    // than wall time. Scheduled seeks reset the reference in
                    // dispatch(), so they are not mistaken for elapsed beats.
                    forward = delta;
                } else if (delta < 0.0 && d.loopActive.load()) {
                    const TrackDataPtr track = d.track();
                    if (track) {
                        const double loopStart =
                            transitionBeatAtSec(file, *track,
                                                d.loopStartSec.load());
                        const double loopEnd =
                            transitionBeatAtSec(file, *track,
                                                d.loopEndSec.load());
                        const double wrapped =
                            (loopEnd - lastDeckTrackBeat) +
                            (currentTrackBeat - loopStart);
                        if (loopEnd > loopStart && wrapped >= 0.0 &&
                            lastDeckTrackBeat >= loopStart - 0.05 &&
                            lastDeckTrackBeat <= loopEnd + 0.05 &&
                            currentTrackBeat >= loopStart - 0.05 &&
                            currentTrackBeat <= loopEnd + 0.05)
                            forward = wrapped;
                    }
                }
                deckBeat += forward;
            }
            lastDeckTrackBeat = currentTrackBeat;
            haveDeckTrackBeat = true;
            const double effective = d.effectiveBpm();
            if (effective > 0.0) timelineBpm = effective;
        } else if (!haveLastBeat) {
            timelineRunning = false;
        }
        lastBeat = std::max({lastBeat, wallBeat, deckBeat});
        return lastBeat;
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
            case ControlId::Filter: return d.filter.load();
            default:                return 0.0;
        }
    }

    void dispatch(Role role, ControlId id, double value) {
        ControlEvent e;
        e.deck = physicalDeck(role);
        e.id = id;
        e.value = (id == ControlId::Crossfader) ? xfaderRoleToPhysical(value) : value;
        bus->dispatch(e, Origin::Replay);
        const bool previewControl =
            id == ControlId::Cue ||
            (id >= ControlId::HotCue1 && id <= ControlId::HotCue8) ||
            (id >= ControlId::TransitionCue1 &&
             id <= ControlId::TransitionCue8) ||
            (id >= ControlId::SavedLoop1 && id <= ControlId::SavedLoop8);
        if (role == Role::ToDeck && previewControl) {
            if (value >= 0.5 && engine->deck(toDeck).previewActive())
                incomingPreviewControl = id;
            else if (value < 0.5 && incomingPreviewControl == id)
                incomingPreviewControl = ControlId::Count;
        }
        if (role == Role::FromDeck) {
            // Scheduled transport actions may seek the outgoing track. They
            // are events *on* the transition timeline, not elapsed time, so
            // reset the deck-position reference after dispatch.
            switch (id) {
            case ControlId::Play:
            case ControlId::Stop:
            case ControlId::Cue:
            case ControlId::TempoSync:
            case ControlId::HotCue1:
            case ControlId::HotCue2:
            case ControlId::HotCue3:
            case ControlId::HotCue4:
            case ControlId::HotCue5:
            case ControlId::HotCue6:
            case ControlId::HotCue7:
            case ControlId::HotCue8:
            case ControlId::TransitionCue1:
            case ControlId::TransitionCue2:
            case ControlId::TransitionCue3:
            case ControlId::TransitionCue4:
            case ControlId::TransitionCue5:
            case ControlId::TransitionCue6:
            case ControlId::TransitionCue7:
            case ControlId::TransitionCue8:
            case ControlId::SavedLoop1:
            case ControlId::SavedLoop2:
            case ControlId::SavedLoop3:
            case ControlId::SavedLoop4:
            case ControlId::SavedLoop5:
            case ControlId::SavedLoop6:
            case ControlId::SavedLoop7:
            case ControlId::SavedLoop8:
            case ControlId::BeatJump:
            case ControlId::PlatterScratch:
                if (const TrackDataPtr track = engine->deck(fromDeck).track())
                    lastDeckTrackBeat = transitionBeatAtSec(
                        file, *track,
                        engine->deck(fromDeck).positionSec());
                else
                    lastDeckTrackBeat =
                        engine->deck(fromDeck).beatPosition();
                haveDeckTrackBeat = engine->deck(fromDeck).playing.load();
                wallBeat = std::max(wallBeat, lastBeat);
                break;
            default:
                break;
            }
        }
        if (role == Role::FromDeck && id == ControlId::Tempo) {
            const double effective = engine->deck(fromDeck).effectiveBpm();
            if (effective > 0.0) timelineBpm = effective;
        }
    }

    void fireFinal(const ScheduledEvent& s) {
        // A standalone ToDeck PLAY is authoritative transport: return to the
        // recorded anchor first if the incoming deck was disturbed after
        // priming. PLAY while CUE/HOT CUE/CUSTOM is held is different: it
        // latches that already-advancing preview exactly where the DJ pressed
        // PLAY, and must not rewind it to the preview's starting anchor.
        if (s.e.role == Role::ToDeck && s.e.control == ControlId::Play &&
            s.e.value >= 0.5) {
            Deck& incoming = engine->deck(toDeck);
            const bool latchingPreview =
                incomingPreviewControl != ControlId::Count &&
                incoming.previewActive();
            if (!latchingPreview) {
                if (TrackDataPtr t = incoming.track())
                    incoming.seekSec(transitionSecAtBeat(
                        file, *t, file.anchorToBeat));
            }
            dispatch(s.e.role, s.e.control, s.e.value);
            incomingPreviewControl = ControlId::Count;
            return;
        }
        dispatch(s.e.role, s.e.control, s.e.value);
    }

    void restorePreStateTransportAtAnchor() {
        const auto restoreCueAndLoop = [this](Deck& deck,
                                          const GvtInitialState& state) {
            const TrackDataPtr track = deck.track();
            if (!track) return;
            deck.cuePointSec.store(transitionSecAtBeat(
                file, *track, state.cueBeat));
            deck.loopStartSec.store(transitionSecAtBeat(
                file, *track, state.loopStartBeat));
            deck.loopEndSec.store(transitionSecAtBeat(
                file, *track, state.loopEndBeat));
            deck.loopActive.store(state.loopActive &&
                                  state.loopEndBeat > state.loopStartBeat);
            if (state.quantizeCaptured)
                deck.quantizeHotCues.store(state.quantize);
        };

        Deck& incoming = engine->deck(toDeck);
        bus->dispatch({toDeck, ControlId::Stop, 1.0}, Origin::Replay);
        if (file.initialComplete && file.initialTo.captured) {
            if (TrackDataPtr track = incoming.track())
                incoming.seekSec(transitionSecAtBeat(
                    file, *track, file.initialTo.positionBeat));
            restoreCueAndLoop(incoming, file.initialTo);
            restoreCueAndLoop(engine->deck(fromDeck), file.initialFrom);
            // If the incoming track was already rolling when recording
            // began, that is part of the pre-transition transport state. It
            // must start at beat zero even when there is no later PLAY event.
            if (file.initialTo.playing)
                bus->dispatch({toDeck, ControlId::Play, 1.0}, Origin::Replay);
        } else if (TrackDataPtr track = incoming.track()) {
            incoming.seekSec(transitionSecAtBeat(
                file, *track, file.anchorToBeat));
        }
    }
};

} // namespace gvt

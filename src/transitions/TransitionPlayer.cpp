// TransitionPlayer — beat-clock replay scheduler (Perform) and prompt/score
// engine (Tutorial). Runs on a ~5 ms QTimer, GUI thread.
// Owner: claude-transitions.
#include "TransitionEngine.h"
#include "TransitionImpls.h"
#include "TransitionPlayerExt.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <map>

namespace gvt {

namespace {

constexpr int    kTickMs        = 5;
constexpr double kGraceBeats    = 1.0;  // after the last event before finished()
constexpr double kTutorialLead  = 8.0;  // longer runway, especially for buttons
constexpr double kTutorialMiss  = 4.0;  // auto-advance past events this late

// Mode side-table: the pinned TransitionEngine.h declares PlayerMode but no
// setter, so the mode is stashed here keyed by player instance (see
// TransitionPlayerExt.h). GUI-thread only, like the player itself.
std::map<const TransitionPlayer*, PlayerMode>& modeTable() {
    static std::map<const TransitionPlayer*, PlayerMode> t;
    return t;
}

std::map<const TransitionPlayer*, bool>& preserveOutgoingTempoTable() {
    static std::map<const TransitionPlayer*, bool> t;
    return t;
}

void prependInitialState(std::vector<GvtEvent>& events,
                         const GvtInitialState& state, Role role,
                         bool complete, bool includeTempo = true) {
    if (!state.captured) return;
    std::vector<GvtEvent> setup;
    const auto add = [&setup, role](ControlId id, double value) {
        setup.push_back(GvtEvent {0.0, role, id, value, Curve::Step});
    };
    if (includeTempo)
        add(ControlId::Tempo, state.tempoRatio);
    add(ControlId::Fader, state.fader);
    add(ControlId::EqLow, state.eqLow);
    add(ControlId::EqMid, state.eqMid);
    add(ControlId::EqHigh, state.eqHigh);
    add(ControlId::Filter, state.filter);
    if (complete) {
        if (state.quantizeCaptured)
            add(ControlId::Quantize, state.quantize ? 1.0 : 0.0);
        add(ControlId::FxType, state.fxType);
        add(ControlId::FxOn, state.fxOn ? 1.0 : 0.0);
        add(ControlId::FxWet, state.fxWet);
        add(ControlId::FxBeats, state.fxBeats);
        add(ControlId::StemVocals, state.stemVocals);
        add(ControlId::StemMelody, state.stemMelody);
        add(ControlId::StemBass, state.stemBass);
        add(ControlId::StemDrums, state.stemDrums);
    }
    events.insert(events.begin(), setup.begin(), setup.end());
}

double replayTempoRatio(const GvtInitialState& state,
                        const GvtTrackRef& recorded,
                        const TrackDataPtr& loaded,
                        double fallbackBpm = 0.0) {
    if (!loaded || loaded->bpm <= 0.0) return state.tempoRatio;
    const double effectiveBpm = fallbackBpm > 0.0
                                    ? fallbackBpm
                                    : recorded.bpm * state.tempoRatio;
    return effectiveBpm > 0.0 ? effectiveBpm / loaded->bpm
                              : state.tempoRatio;
}

double replayTempoEvent(double recordedRatio,
                        const GvtTrackRef& recorded,
                        const TrackDataPtr& loaded) {
    if (!loaded || loaded->bpm <= 0.0 || recorded.bpm <= 0.0)
        return recordedRatio;
    const double recordedEffectiveBpm = recorded.bpm * recordedRatio;
    return recordedEffectiveBpm > 0.0
               ? recordedEffectiveBpm / loaded->bpm : recordedRatio;
}

} // namespace

TransitionPlayer::TransitionPlayer(ControlBus* bus, AudioEngine* engine,
                                   QObject* parent)
    : QObject(parent), impl_(new Impl) {
    Impl& im = *impl_;
    im.bus = bus;
    im.engine = engine;
    im.timer.setInterval(kTickMs);
    im.timer.setTimerType(Qt::PreciseTimer);

    connect(&im.timer, &QTimer::timeout, this, [this] {
        Impl& im2 = *impl_;
        if (!im2.active) return;
        const double rel = im2.currentRel();

        // PRIME may sit armed for many bars. Reassert transport-dependent
        // pre-state at the actual entry boundary so an incoming deck touched
        // after arming cannot make replay skip its recorded cue position.
        if (im2.mode == PlayerMode::Perform &&
            !im2.preStateTransportApplied && rel >= 0.0) {
            im2.restorePreStateTransportAtAnchor();
            im2.preStateTransportApplied = true;
        }

        for (size_t i = 0; i < im2.sched.size(); ++i) {
            if (im2.done[i]) continue;
            ScheduledEvent& s = im2.sched[i];
            const double due = s.e.beat;

            if (im2.mode == PlayerMode::Tutorial) {
                if (!im2.prompted[i] && rel >= due - kTutorialLead) {
                    im2.prompted[i] = 1;
                    emit tutorialPrompt(s.e, due - rel);
                }
                if (rel > due + kTutorialMiss) { // human missed it — advance
                    im2.done[i] = 1;
                    emit tutorialScored(s.e, kTutorialMiss, 0.0);
                }
                continue;
            }

            if (rel >= due) {
                im2.fireFinal(s);
                im2.done[i] = 1;
            } else if (scheduledIsGlide(s) && rel > s.startBeat) {
                im2.dispatch(s.e.role, s.e.control, glideValueAt(s, rel));
            }
        }

        // Report negative pre-anchor time too. The UI clamps its progress bar,
        // while Tutorial uses the raw value for an honest 8-beat countdown.
        emit progressChanged(rel, im2.totalBeats);

        if (rel >= im2.totalBeats + kGraceBeats) {
            im2.active = false;
            im2.timer.stop();
            emit finished(true);
        }
    });

    // Tutorial scoring: match the human's live events against pending ones.
    connect(bus, &ControlBus::eventDispatched, this,
            [this](const ControlEvent& e, Origin origin) {
        Impl& im2 = *impl_;
        if (!im2.active || im2.mode != PlayerMode::Tutorial) return;
        if (origin != Origin::Ui && origin != Origin::Midi) return;

        const double rel = im2.currentRel();
        int best = -1;
        double bestDist = kTutorialMiss;  // only score within the miss window
        for (size_t i = 0; i < im2.sched.size(); ++i) {
            if (im2.done[i]) continue;
            const ScheduledEvent& s = im2.sched[i];
            if (im2.physicalDeck(s.e.role) != e.deck || s.e.control != e.id)
                continue;
            const double dist = std::fabs(rel - s.e.beat);
            if (dist <= bestDist) { bestDist = dist; best = (int)i; }
        }
        if (best < 0) return;
        ScheduledEvent& s = im2.sched[best];
        const double beatError = rel - s.e.beat;
        const double expectedValue =
            s.e.control == ControlId::Crossfader
                ? im2.xfaderRoleToPhysical(s.e.value) : s.e.value;
        const double valueError = controlIsTrigger(s.e.control)
                                      ? 0.0
                                      : std::fabs(e.value - expectedValue);
        // Keep continuous controls highlighted while the student moves them.
        // A first nudge identifies the right knob but is not the completed
        // gesture; MidiEngine still feeds its live value to the tutorial's
        // animated target marker until it is genuinely close.
        if (!controlIsTrigger(s.e.control)) {
            const double tolerance = s.e.control == ControlId::Tempo
                                         ? 0.002 : 0.04;
            if (valueError > tolerance)
                return;
        }
        im2.done[best] = 1;
        emit tutorialScored(s.e, beatError, valueError);
    });
}

TransitionPlayer::~TransitionPlayer() {
    modeTable().erase(this); // a heap-reused address must not inherit our mode
    preserveOutgoingTempoTable().erase(this);
}

bool TransitionPlayer::arm(const GvtFile& f, int fromDeck, bool startNow,
                           QString* error) {
    Impl& im = *impl_;
    if (im.active) {
        if (error) *error = QStringLiteral("player already active");
        return false;
    }
    if (!f.unsupportedRequirements.isEmpty()) {
        if (error) *error = QStringLiteral("unsupported transition capabilities: %1")
                                .arg(f.unsupportedRequirements.join(", "));
        return false;
    }
    const int toDeck = (fromDeck == 0) ? 1 : 0;
    if (!im.engine->deck(toDeck).track()) {
        if (error) *error =
            QStringLiteral("no track loaded on the incoming deck (%1)").arg(toDeck);
        return false;
    }

    im.file = f;
    im.fromDeck = fromDeck;
    im.toDeck = toDeck;
    if (startNow) {
        const Deck& outgoing = im.engine->deck(fromDeck);
        const TrackDataPtr track = outgoing.track();
        im.anchorFrom = track
            ? transitionBeatAtSec(f, *track, outgoing.positionSec())
            : outgoing.beatPosition();
    } else {
        im.anchorFrom = f.anchorFromBeat;
    }

    auto it = modeTable().find(this);
    im.mode = (it != modeTable().end()) ? it->second : PlayerMode::Perform;
    const auto preserveIt = preserveOutgoingTempoTable().find(this);
    const bool preserveOutgoingSetupTempo =
        preserveIt != preserveOutgoingTempoTable().end() &&
        preserveIt->second;

    std::vector<GvtEvent> events;
    events.reserve(f.events.size());
    std::copy_if(f.events.begin(), f.events.end(),
                 std::back_inserter(events), [](const GvtEvent& event) {
                     // Old files remain readable, but gain staging is no
                     // longer applied as part of a transition.
                     return event.control != ControlId::Trim;
                 });
    const auto prepareCues = [&](Role role, int physicalDeck,
                                  const TrackDataPtr& track) {
        std::array<double, 8> seconds {
            -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
        const auto cueSlots = transitionCueSlots(f, role);
        for (int slot = 0; slot < 8; ++slot)
            if (cueSlots[static_cast<std::size_t>(slot)] && track)
                seconds[static_cast<std::size_t>(slot)] = transitionSecAtBeat(
                    f, *track,
                    cueSlots[static_cast<std::size_t>(slot)]->trackBeat);
        im.engine->deck(physicalDeck).setTransitionCues(seconds);
        return cueSlots;
    };
    const auto fromCueSlots = prepareCues(
        Role::FromDeck, fromDeck, im.engine->deck(fromDeck).track());
    const auto toCueSlots = prepareCues(
        Role::ToDeck, toDeck, im.engine->deck(toDeck).track());
    for (GvtEvent& event : events) {
        if (event.cueId.isEmpty() || event.role == Role::Mixer) continue;
        const auto& cueSlots = event.role == Role::FromDeck
                                ? fromCueSlots : toCueSlots;
        const auto found = std::find_if(
            cueSlots.begin(), cueSlots.end(), [&event](const TransitionHotCue* cue) {
                return cue && cue->id == event.cueId;
            });
        if (found == cueSlots.end()) {
            if (error) *error = QStringLiteral("timeline refers to unallocated cue '%1'")
                                    .arg(event.cueId);
            return false;
        }
        const int slot = static_cast<int>(
            std::distance(cueSlots.begin(), found));
        event.control = static_cast<ControlId>(
            static_cast<int>(ControlId::TransitionCue1) + slot);
    }
    // A library re-scan or manual regrid can change the loaded track's native
    // BPM after recording. Preserve every recorded effective BPM, including
    // later tempo moves, rather than blindly replaying a now-wrong ratio.
    for (GvtEvent& event : events) {
        if (event.control != ControlId::Tempo) continue;
        if (event.role == Role::FromDeck)
            event.value = replayTempoEvent(
                event.value, f.from, im.engine->deck(fromDeck).track());
        else if (event.role == Role::ToDeck)
            event.value = replayTempoEvent(
                event.value, f.to, im.engine->deck(toDeck).track());
    }
    // Setup snapshots are replay actions, not tutorial gestures.  In Perform
    // mode they fire at the anchor (including when PRIME has waited there), so
    // the outgoing deck starts at the BPM/EQ used by the recording.
    if (im.mode == PlayerMode::Perform) {
        GvtInitialState fromSetup = f.initialFrom;
        fromSetup.tempoRatio = replayTempoRatio(
            fromSetup, f.from, im.engine->deck(fromDeck).track(), f.masterBpm);
        prependInitialState(events, fromSetup, Role::FromDeck,
                            f.initialComplete,
                            !preserveOutgoingSetupTempo);
        if (f.initialComplete) {
            GvtInitialState toSetup = f.initialTo;
            toSetup.tempoRatio = replayTempoRatio(
                toSetup, f.to, im.engine->deck(toDeck).track());
            prependInitialState(events, toSetup, Role::ToDeck, true);
            if (f.initialMixerCaptured) {
                events.insert(events.begin(),
                              GvtEvent {0.0, Role::Mixer,
                                        ControlId::Crossfader,
                                        f.initialCrossfader, Curve::Step});
            }
        }
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const GvtEvent& a, const GvtEvent& b) { return a.beat < b.beat; });

    im.sched = buildSchedule(events, [&im](Role r, ControlId id) {
        return im.engineValue(r, id);
    });
    im.done.assign(im.sched.size(), 0);
    im.prompted.assign(im.sched.size(), 0);
    im.totalBeats = events.empty() ? 0.0 : events.back().beat;
    im.haveLastBeat = false;
    im.lastBeat = 0.0;
    im.timelineStarted = false;
    im.timelineRunning = false;
    im.timelineBpm = f.masterBpm;
    im.wallBeat = 0.0;
    im.deckBeat = 0.0;
    im.lastDeckTrackBeat = 0.0;
    im.haveDeckTrackBeat = false;
    im.lastTimelineNs = 0;
    if (startNow) {
        const Deck& outgoing = im.engine->deck(fromDeck);
        im.timelineStarted = true;
        im.timelineRunning = outgoing.playing.load();
        im.haveLastBeat = im.timelineRunning;
        if (const TrackDataPtr track = outgoing.track())
            im.lastDeckTrackBeat = transitionBeatAtSec(
                f, *track, outgoing.positionSec());
        else
            im.lastDeckTrackBeat = outgoing.beatPosition();
        im.haveDeckTrackBeat = true;
        const double effective = outgoing.effectiveBpm();
        if (effective > 0.0) im.timelineBpm = effective;
        im.timelineClock.start();
    }
    im.preStateTransportApplied = false;
    im.incomingPreviewControl = ControlId::Count;

    im.active = true;
    im.timer.start();
    return true;
}

void TransitionPlayer::abort() {
    Impl& im = *impl_;
    if (!im.active) return;
    im.timer.stop();
    im.active = false;
    emit finished(false);
}

bool TransitionPlayer::isActive() const { return impl_->active; }

void transitionPlayerSetMode(TransitionPlayer* player, PlayerMode mode) {
    modeTable()[player] = mode;
}

void transitionPlayerPreserveOutgoingSetupTempo(TransitionPlayer* player,
                                                bool preserve) {
    preserveOutgoingTempoTable()[player] = preserve;
}

} // namespace gvt

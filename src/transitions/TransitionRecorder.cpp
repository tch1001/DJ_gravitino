// TransitionRecorder — logs Human-origin ControlBus events beat-stamped
// against the outgoing deck's beatgrid. Owner: claude-transitions.
#include "TransitionEngine.h"
#include "TransitionImpls.h"

#include <QDate>
#include <cmath>
#include <vector>

namespace gvt {

namespace {

constexpr double kCoalesceBeats = 0.05;  // merge same-key events closer than this
constexpr double kLinearEps     = 0.01;  // value tolerance for thinning

Role roleForDeck(DeckId deck, int fromDeck) {
    if (deck == kNoDeck) return Role::Mixer;
    return (deck == fromDeck) ? Role::FromDeck : Role::ToDeck;
}

GvtInitialState captureDeckState(const Deck& deck) {
    GvtInitialState state;
    const TrackDataPtr track = deck.track();
    if (!track) return state;

    state.captured = true;
    state.playing = deck.playing.load();
    state.positionBeat = deck.beatPosition();
    const double cueSec = deck.cuePointSec.load();
    state.cueBeat = std::isfinite(cueSec) && cueSec >= 0.0
                        ? track->beatAtSec(cueSec)
                        : state.positionBeat;
    state.tempoRatio = deck.tempoRatio.load();
    state.fader = deck.fader.load();
    state.trimCaptured = false;
    state.eqLow = deck.eqLow.load();
    state.eqMid = deck.eqMid.load();
    state.eqHigh = deck.eqHigh.load();
    state.filter = deck.filter.load();
    state.quantizeCaptured = true;
    state.quantize = deck.quantizeHotCues.load();
    state.loopActive = deck.loopActive.load();
    const double loopStart = deck.loopStartSec.load();
    const double loopEnd = deck.loopEndSec.load();
    state.loopStartBeat = std::isfinite(loopStart) && loopStart >= 0.0
                              ? track->beatAtSec(loopStart)
                              : state.positionBeat;
    state.loopEndBeat = std::isfinite(loopEnd) && loopEnd >= 0.0
                            ? track->beatAtSec(loopEnd)
                            : state.positionBeat;
    state.fxType = deck.fxType.load();
    state.fxOn = deck.fxOn.load();
    state.fxWet = deck.fxWet.load();
    state.fxBeats = deck.fxBeats.load();
    state.stemVocals = deck.stemVocals.load();
    state.stemMelody = deck.stemMelody.load();
    state.stemBass = deck.stemBass.load();
    state.stemDrums = deck.stemDrums.load();
    return state;
}

} // namespace

TransitionRecorder::TransitionRecorder(ControlBus* bus, AudioEngine* engine,
                                       QObject* parent)
    : QObject(parent), impl_(new Impl) {
    impl_->bus = bus;
    impl_->engine = engine;

    connect(bus, &ControlBus::eventDispatched, this,
            [this](const ControlEvent& e, Origin origin) {
        Impl& im = *impl_;
        if (!im.recording) return;

        const double beat = im.currentBeat(&e);

        // Capture the TO-deck anchor the moment it first starts playing.
        if (!im.toAnchorSet) {
            const bool playEvent = (e.deck == im.toDeck &&
                                    e.id == ControlId::Play && e.value >= 0.5);
            if (playEvent || im.engine->deck(im.toDeck).playing.load()) {
                im.toAnchorBeat = im.engine->deck(im.toDeck).beatPosition();
                im.toAnchorSet = true;
            }
        }

        // Only human origins are recorded — never Replay or System.
        if (origin != Origin::Ui && origin != Origin::Midi) return;

        // Performance-pad controls are host gesture hints rather than audio
        // events. Preserve the selected layer and attach it to the immediately
        // following audible state change so Tutorial can teach the button that
        // caused PLAY/LOOP/FX instead of only describing the resulting state.
        if (e.id >= ControlId::PerformancePad1 &&
            e.id <= ControlId::PerformancePad8) {
            im.gesturePending = true;
            im.gestureDeck = e.deck;
            im.gestureControl = e.id;
            im.gesturePadMode = static_cast<int>(std::lround(e.value));
            return;
        }

        // Jog nudges are transient rate bends; headphone monitoring is local
        // to the DJ and never part of the audible transition. Skip both.
        if (e.id == ControlId::Jog ||
            e.id == ControlId::PlatterScratch ||
            e.id == ControlId::PlatterTouch ||
            e.id == ControlId::BrowseNavigate ||
            e.id == ControlId::BrowseSelect ||
            e.id == ControlId::PerformancePadMode ||
            (e.id >= ControlId::PerformancePad1 &&
             e.id <= ControlId::PerformancePad8) ||
            e.id == ControlId::HeadphoneCue ||
            e.id == ControlId::MasterCue ||
            e.id == ControlId::HeadphoneMix ||
            e.id == ControlId::Trim)
            return;

        if (e.deck >= 0 && e.deck < kNumDecks &&
            e.id >= ControlId::HotCue1 && e.id <= ControlId::HotCue8) {
            const int pad = static_cast<int>(e.id) -
                            static_cast<int>(ControlId::HotCue1);
            const TrackDataPtr track = im.engine->deck(e.deck).track();
            if (track && std::isfinite(track->hotCues[pad]) &&
                track->hotCues[pad] >= 0.0) {
                auto& mappings = e.deck == im.fromDeck
                                     ? im.fromHotCueBeats : im.toHotCueBeats;
                // Preserve the assignment at the first recorded use. A pad
                // edited later in the same take must not rewrite what the
                // earlier gesture actually meant.
                if (!hotCueBeatIsMapped(
                        mappings[static_cast<std::size_t>(pad)]))
                    mappings[static_cast<std::size_t>(pad)] =
                        track->beatAtSec(track->hotCues[pad]);
            }
        }

        GvtEvent g;
        g.beat = std::max(0.0, beat);
        g.role = roleForDeck(e.deck, im.fromDeck);
        g.control = e.id;
        // Crossfader is stored in ROLE space (0 = from-deck, 1 = to-deck) so
        // a transition replays correctly when the pair is loaded swapped.
        g.value = (e.id == ControlId::Crossfader && im.fromDeck != 0)
                      ? 1.0 - e.value : e.value;
        g.curve = Curve::Step;
        if (im.gesturePending && im.gestureDeck == e.deck) {
            g.gestureControl = im.gestureControl;
            g.gesturePadMode = im.gesturePadMode;
            im.gesturePending = false;
            im.gestureDeck = kNoDeck;
            im.gestureControl = ControlId::Count;
            im.gesturePadMode = -1;
        }

        // Coalesce dense runs of the same continuous (role, control) into
        // fixed 0.05-beat buckets: update the bucket's value but NEVER move
        // its beat forward — a rolling window would swallow a whole slow
        // gesture into one snap. Negative deltas (clock jumps) start a new
        // event instead of corrupting an old bucket.
        if (!controlIsTrigger(g.control)) {
            for (auto it = im.events.rbegin(); it != im.events.rend(); ++it) {
                if (it->role != g.role || it->control != g.control) continue;
                const double delta = g.beat - it->beat;
                if (delta >= 0.0 && delta < kCoalesceBeats) {
                    it->value = g.value;
                    if (g.gestureControl != ControlId::Count) {
                        it->gestureControl = g.gestureControl;
                        it->gesturePadMode = g.gesturePadMode;
                    }
                    emit eventCaptured((int)im.events.size());
                    return;
                }
                break;
            }
        }
        im.events.push_back(g);
        emit eventCaptured((int)im.events.size());
    });
}

TransitionRecorder::~TransitionRecorder() = default;

void TransitionRecorder::start(int fromDeck) {
    Impl& im = *impl_;
    im.fromDeck = fromDeck;
    im.toDeck = (fromDeck == 0) ? 1 : 0;
    im.anchorBeat = im.engine->deck(fromDeck).beatPosition();
    im.masterBpm = im.engine->deck(fromDeck).effectiveBpm();
    im.initialFrom = captureDeckState(im.engine->deck(fromDeck));
    im.initialTo = captureDeckState(im.engine->deck(im.toDeck));
    const double physicalCrossfader = im.engine->crossfader.load();
    im.initialCrossfader = fromDeck == 0 ? physicalCrossfader
                                         : 1.0 - physicalCrossfader;
    im.toAnchorSet = false;
    im.toAnchorBeat = 0.0;
    // If the incoming deck is already rolling, its anchor is NOW — waiting for
    // the first bus event would record it beats late.
    if (im.engine->deck(im.toDeck).playing.load()) {
        im.toAnchorBeat = im.engine->deck(im.toDeck).beatPosition();
        im.toAnchorSet = true;
    }
    im.haveLastBeat = im.engine->deck(fromDeck).playing.load();
    im.lastBeat = 0.0;
    im.timelineRunning = im.haveLastBeat;
    im.timelineBpm = im.masterBpm;
    im.wallBeat = 0.0;
    im.deckBeat = 0.0;
    im.lastDeckTrackBeat = im.engine->deck(fromDeck).beatPosition();
    im.haveDeckTrackBeat = true;
    im.timelineClock.start();
    im.lastTimelineNs = 0;
    im.fromHotCueBeats.fill(kUnmappedHotCueBeat);
    im.toHotCueBeats.fill(kUnmappedHotCueBeat);
    im.events.clear();
    im.gesturePending = false;
    im.gestureDeck = kNoDeck;
    im.gestureControl = ControlId::Count;
    im.gesturePadMode = -1;
    im.recording = true;
}

bool TransitionRecorder::isRecording() const { return impl_->recording; }

void TransitionRecorder::cancel() {
    impl_->recording = false;
    impl_->events.clear();
    impl_->gesturePending = false;
}

GvtFile TransitionRecorder::finish() {
    Impl& im = *impl_;
    im.recording = false;

    GvtFile f;
    f.version = 1;
    f.created = QDate::currentDate().toString(Qt::ISODate);

    auto fillRef = [&](GvtTrackRef& ref, int deckIdx) {
        if (TrackDataPtr t = im.engine->deck(deckIdx).track()) {
            ref.title = t->title;
            ref.artist = t->artist;
            ref.bpm = t->bpm;
            ref.durationSec = t->durationSec;
            ref.fingerprint = t->fingerprint;
        }
    };
    fillRef(f.from, im.fromDeck);
    fillRef(f.to, im.toDeck);

    f.anchorFromBeat = im.anchorBeat;
    if (!im.toAnchorSet) // fall back: TO-deck position at finish
        im.toAnchorBeat = im.engine->deck(im.toDeck).beatPosition();
    f.anchorToBeat = im.toAnchorBeat;
    f.masterBpm = im.masterBpm;
    f.initialComplete = true;
    f.initialFrom = im.initialFrom;
    f.initialTo = im.initialTo;
    f.initialMixerCaptured = true;
    f.initialCrossfader = im.initialCrossfader;
    f.fromHotCueBeats = im.fromHotCueBeats;
    f.toHotCueBeats = im.toHotCueBeats;

    // Sort, then thin per-key runs: drop intermediate continuous points that
    // sit on the line between their kept neighbors; survivors past the first
    // of a run become Linear. Triggers are always kept (Step).
    std::stable_sort(im.events.begin(), im.events.end(),
                     [](const GvtEvent& a, const GvtEvent& b) { return a.beat < b.beat; });

    std::vector<bool> keep(im.events.size(), true);
    // Group indices per (role, control) for continuous controls.
    for (size_t i = 0; i < im.events.size(); ++i) {
        const GvtEvent& first = im.events[i];
        if (controlIsTrigger(first.control)) continue;
        // Collect the whole run for this key starting from its first event.
        bool isFirstOfKey = true;
        for (size_t j = 0; j < i; ++j)
            if (im.events[j].role == first.role &&
                im.events[j].control == first.control) { isFirstOfKey = false; break; }
        if (!isFirstOfKey) continue;

        std::vector<size_t> run;
        for (size_t j = i; j < im.events.size(); ++j)
            if (im.events[j].role == first.role &&
                im.events[j].control == first.control)
                run.push_back(j);
        if (run.size() < 2) continue;

        // Greedy thinning: anchor at last kept point, drop middles that are
        // linear (within eps) between the anchor and the next point.
        size_t a = 0;
        for (size_t m = 1; m + 1 < run.size(); ++m) {
            const GvtEvent& pa = im.events[run[a]];
            const GvtEvent& pm = im.events[run[m]];
            const GvtEvent& pb = im.events[run[m + 1]];
            const double span = pb.beat - pa.beat;
            double expect = pb.value;
            if (span > 1e-9)
                expect = pa.value + (pb.value - pa.value) * (pm.beat - pa.beat) / span;
            if (std::fabs(pm.value - expect) <= kLinearEps)
                keep[run[m]] = false;
            else
                a = m;
        }
        // Survivors after the first of the run glide linearly into place —
        // unless the value jumped (a toggle like a stem mute or a fader slam
        // must snap on replay, not fade across the gap since the last event).
        bool firstKept = true;
        double prevKeptValue = 0.0;
        for (size_t idx : run) {
            if (!keep[idx]) continue;
            const bool bigJump =
                !firstKept &&
                std::fabs(im.events[idx].value - prevKeptValue) > 0.45;
            im.events[idx].curve =
                (firstKept || bigJump) ? Curve::Step : Curve::Linear;
            prevKeptValue = im.events[idx].value;
            firstKept = false;
        }
    }

    for (size_t i = 0; i < im.events.size(); ++i)
        if (keep[i]) f.events.push_back(im.events[i]);

    // Hot-cue events only name a pad. Capture the pad's actual track beat so
    // Tutorial can verify that a later library/controller setup points to the
    // same musical moment. This is captured at finish so a cue first assigned
    // during the recording is included too.
    for (const GvtEvent& event : f.events) {
        if (event.control < ControlId::HotCue1 ||
            event.control > ControlId::HotCue8 || event.role == Role::Mixer)
            continue;
        const int pad = static_cast<int>(event.control) -
                        static_cast<int>(ControlId::HotCue1);
        const int physicalDeck = event.role == Role::FromDeck
                                     ? im.fromDeck : im.toDeck;
        const TrackDataPtr track = im.engine->deck(physicalDeck).track();
        if (!track) continue;
        const double sec = track->hotCues[pad];
        if (!std::isfinite(sec) || sec < 0.0) continue;
        auto& mappings = event.role == Role::FromDeck
                             ? f.fromHotCueBeats : f.toHotCueBeats;
        if (!hotCueBeatIsMapped(mappings[static_cast<std::size_t>(pad)]))
            mappings[static_cast<std::size_t>(pad)] = track->beatAtSec(sec);
    }

    // Keep timing and tempo at the precision produced by the audio engine.
    // Coarser rounding made a 129.91-BPM incoming track drift audibly when a
    // 14-bit tempo ratio was reduced to only three decimals. Channel/fader
    // controls remain at 0.001 because that is their actual UI resolution.
    auto q = [](double v, double step) { return std::round(v / step) * step; };
    constexpr double kPrecise = 0.000001;
    f.from.bpm = q(f.from.bpm, kPrecise);
    f.to.bpm = q(f.to.bpm, kPrecise);
    f.from.durationSec = q(f.from.durationSec, 0.01);
    f.to.durationSec = q(f.to.durationSec, 0.01);
    f.anchorFromBeat = q(f.anchorFromBeat, kPrecise);
    f.anchorToBeat = q(f.anchorToBeat, kPrecise);
    f.masterBpm = q(f.masterBpm, kPrecise);
    const auto quantizeState = [&q](GvtInitialState& state) {
        constexpr double precise = 0.000001;
        state.positionBeat = q(state.positionBeat, precise);
        state.cueBeat = q(state.cueBeat, precise);
        state.tempoRatio = q(state.tempoRatio, precise);
        state.fader = q(state.fader, 0.001);
        state.trim = q(state.trim, 0.001);
        state.eqLow = q(state.eqLow, 0.001);
        state.eqMid = q(state.eqMid, 0.001);
        state.eqHigh = q(state.eqHigh, 0.001);
        state.filter = q(state.filter, 0.001);
        state.loopStartBeat = q(state.loopStartBeat, precise);
        state.loopEndBeat = q(state.loopEndBeat, precise);
        state.fxWet = q(state.fxWet, 0.001);
        state.fxBeats = q(state.fxBeats, 0.001);
        state.stemVocals = q(state.stemVocals, 0.001);
        state.stemMelody = q(state.stemMelody, 0.001);
        state.stemBass = q(state.stemBass, 0.001);
        state.stemDrums = q(state.stemDrums, 0.001);
    };
    quantizeState(f.initialFrom);
    quantizeState(f.initialTo);
    f.initialCrossfader = q(f.initialCrossfader, 0.001);
    for (auto* mappings : {&f.fromHotCueBeats, &f.toHotCueBeats})
        for (double& beat : *mappings)
            if (hotCueBeatIsMapped(beat)) beat = q(beat, kPrecise);
    for (GvtEvent& e : f.events) {
        e.beat = q(e.beat, kPrecise);
        e.value = q(e.value,
                    e.control == ControlId::Tempo ? kPrecise : 0.001);
    }

    im.events.clear();
    return f;
}

} // namespace gvt

// TransitionEngine.h has no same-named .cpp, so AUTOMOC won't moc it on its
// own; this include makes AUTOMOC generate the meta-objects for both
// TransitionRecorder and TransitionPlayer into this TU.
#include "moc_TransitionEngine.cpp"

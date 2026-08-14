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

        const double beat = im.currentBeat();

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

        GvtEvent g;
        g.beat = std::max(0.0, beat);
        g.role = roleForDeck(e.deck, im.fromDeck);
        g.control = e.id;
        g.value = e.value;
        g.curve = Curve::Step;

        // Coalesce dense runs of the same continuous (role, control): if the
        // previous event for this key is closer than kCoalesceBeats, keep only
        // the newest value.
        if (!controlIsTrigger(g.control)) {
            for (auto it = im.events.rbegin(); it != im.events.rend(); ++it) {
                if (it->role != g.role || it->control != g.control) continue;
                if (g.beat - it->beat < kCoalesceBeats) {
                    it->beat = g.beat;
                    it->value = g.value;
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
    im.toAnchorSet = false;
    im.toAnchorBeat = 0.0;
    im.haveLastBeat = false;
    im.lastBeat = 0.0;
    im.events.clear();
    im.recording = true;
}

bool TransitionRecorder::isRecording() const { return impl_->recording; }

void TransitionRecorder::cancel() {
    impl_->recording = false;
    impl_->events.clear();
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
        // Survivors after the first of the run glide linearly into place.
        bool firstKept = true;
        for (size_t idx : run) {
            if (!keep[idx]) continue;
            im.events[idx].curve = firstKept ? Curve::Step : Curve::Linear;
            firstKept = false;
        }
    }

    for (size_t i = 0; i < im.events.size(); ++i)
        if (keep[i]) f.events.push_back(im.events[i]);

    // Quantize for clean, human-editable files: 0.01 for BPM/durations,
    // 0.001 beats (~0.5 ms at 128 BPM) for anchors and event times/values.
    auto q = [](double v, double step) { return std::round(v / step) * step; };
    f.from.bpm = q(f.from.bpm, 0.01); f.to.bpm = q(f.to.bpm, 0.01);
    f.from.durationSec = q(f.from.durationSec, 0.01);
    f.to.durationSec = q(f.to.durationSec, 0.01);
    f.anchorFromBeat = q(f.anchorFromBeat, 0.001);
    f.anchorToBeat = q(f.anchorToBeat, 0.001);
    f.masterBpm = q(f.masterBpm, 0.01);
    for (GvtEvent& e : f.events) { e.beat = q(e.beat, 0.001); e.value = q(e.value, 0.001); }

    im.events.clear();
    return f;
}

} // namespace gvt

// TransitionEngine.h has no same-named .cpp, so AUTOMOC won't moc it on its
// own; this include makes AUTOMOC generate the meta-objects for both
// TransitionRecorder and TransitionPlayer into this TU.
#include "moc_TransitionEngine.cpp"

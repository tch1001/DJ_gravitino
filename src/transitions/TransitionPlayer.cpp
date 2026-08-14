// TransitionPlayer — beat-clock replay scheduler (Perform) and prompt/score
// engine (Tutorial). Runs on a ~5 ms QTimer, GUI thread.
// Owner: claude-transitions.
#include "TransitionEngine.h"
#include "TransitionImpls.h"
#include "TransitionPlayerExt.h"

#include <cmath>
#include <map>

namespace gvt {

namespace {

constexpr int    kTickMs        = 5;
constexpr double kGraceBeats    = 1.0;  // after the last event before finished()
constexpr double kTutorialLead  = 4.0;  // prompt this many beats ahead
constexpr double kTutorialMiss  = 4.0;  // auto-advance past events this late

// Mode side-table: the pinned TransitionEngine.h declares PlayerMode but no
// setter, so the mode is stashed here keyed by player instance (see
// TransitionPlayerExt.h). GUI-thread only, like the player itself.
std::map<const TransitionPlayer*, PlayerMode>& modeTable() {
    static std::map<const TransitionPlayer*, PlayerMode> t;
    return t;
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

        emit progressChanged(std::clamp(rel, 0.0, im2.totalBeats), im2.totalBeats);

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
        im2.done[best] = 1;
        const double beatError = rel - s.e.beat;
        const double valueError = controlIsTrigger(s.e.control)
                                      ? 0.0
                                      : std::fabs(e.value - s.e.value);
        emit tutorialScored(s.e, beatError, valueError);
    });
}

TransitionPlayer::~TransitionPlayer() {
    modeTable().erase(this); // a heap-reused address must not inherit our mode
}

bool TransitionPlayer::arm(const GvtFile& f, int fromDeck, bool startNow,
                           QString* error) {
    Impl& im = *impl_;
    if (im.active) {
        if (error) *error = QStringLiteral("player already active");
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
    im.anchorFrom = startNow ? im.engine->deck(fromDeck).beatPosition()
                             : f.anchorFromBeat;

    auto it = modeTable().find(this);
    im.mode = (it != modeTable().end()) ? it->second : PlayerMode::Perform;

    std::vector<GvtEvent> events = f.events;
    std::stable_sort(events.begin(), events.end(),
                     [](const GvtEvent& a, const GvtEvent& b) { return a.beat < b.beat; });

    im.sched = buildSchedule(events, [&im](Role r, ControlId id) {
        return im.engineValue(r, id);
    });
    im.done.assign(im.sched.size(), 0);
    im.prompted.assign(im.sched.size(), 0);
    im.totalBeats = events.empty() ? 0.0 : events.back().beat;

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

} // namespace gvt

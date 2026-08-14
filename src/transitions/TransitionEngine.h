// PINNED INTERFACE — recorder + beat-clock replay + tutorial scoring.
#pragma once
#include <QObject>
#include "Transition.h"
#include "../audio/AudioEngine.h"

namespace gvt {

// Records Human-origin ControlBus events into a GvtFile, beat-stamped
// against the master (outgoing) deck's beatgrid.
class TransitionRecorder : public QObject {
    Q_OBJECT
public:
    TransitionRecorder(ControlBus* bus, AudioEngine* engine, QObject* parent = nullptr);
    // fromDeck = physical deck of the outgoing track at record start.
    void start(int fromDeck);
    bool isRecording() const;
    // Stop & build the file (fills [from][to][sync][events] from engine state).
    GvtFile finish();
    void cancel();
signals:
    void eventCaptured(int count); // for UI counter
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

enum class PlayerMode {
    Perform,   // fire events onto the bus (Origin::Replay)
    Tutorial   // emit prompts instead; score human's live events
};

// Beat-clock scheduler: waits for the master deck to reach anchor_from, then
// interpolates/fires events. Runs on a ~5ms QTimer, GUI thread.
class TransitionPlayer : public QObject {
    Q_OBJECT
public:
    TransitionPlayer(ControlBus* bus, AudioEngine* engine, QObject* parent = nullptr);
    // fromDeck: physical deck playing the [from] track; the [to] track must
    // already be loaded on the other deck. startNow=true skips waiting for
    // the anchor beat and begins at the current beat.
    bool arm(const GvtFile& f, int fromDeck, bool startNow, QString* error);
    void abort();
    bool isActive() const;
signals:
    void progressChanged(double beatsIn, double beatsTotal);
    void finished(bool completed);
    // Tutorial mode: prompt the human `beatsAhead` before an event is due,
    // and report accuracy per event (beat error, value error).
    void tutorialPrompt(const gvt::GvtEvent& e, double beatsAhead);
    void tutorialScored(const gvt::GvtEvent& e, double beatError, double valueError);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};

} // namespace gvt

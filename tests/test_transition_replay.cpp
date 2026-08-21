#include "audio/AudioEngine.h"
#include "control/ControlBus.h"
#include "transitions/TransitionEngine.h"
#include "transitions/TransitionPlayerExt.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace {
int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                     #condition); \
        ++failures; \
    } \
} while (0)

void spinEvents(int milliseconds = 30)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

gvt::TrackDataPtr makeTrack(double bpm = 120.0)
{
    auto track = std::make_shared<gvt::TrackData>();
    track->durationSec = 8.0;
    track->bpm = bpm;
    track->firstBeatSec = 0.0;
    track->pcm.resize(
        static_cast<std::size_t>(gvt::kSampleRate) * 8U * 2U);
    return track;
}
}

int main(int argc, char** argv)
{
    using namespace gvt;
    QCoreApplication app(argc, argv);
    ControlBus bus;
    AudioEngine engine(&bus);
    engine.deck(0).loadTrack(makeTrack());
    engine.deck(1).loadTrack(makeTrack());
    engine.deck(0).seekSec(0.0);
    engine.deck(0).play();

    GvtFile file;
    file.initialComplete = true;
    file.initialMixerCaptured = true;
    file.anchorFromBeat = 0.0;
    file.anchorToBeat = 2.0; // one second at 120 BPM
    file.masterBpm = 120.0;
    file.initialFrom.captured = true;
    file.initialFrom.playing = true;
    file.initialFrom.loopActive = true;
    file.initialFrom.loopStartBeat = 0.0;
    file.initialFrom.loopEndBeat = 2.0;
    file.initialTo.captured = true;
    file.initialTo.playing = false;
    file.initialTo.positionBeat = 7.0; // deliberately not the play anchor
    file.events.push_back(
        {0.12, Role::ToDeck, ControlId::Play, 1.0, Curve::Step});

    TransitionPlayer player(&bus, &engine);
    QString error;

    // A recorded PLAY always overrides a disturbed live incoming position.
    engine.deck(0).seekSec(0.99); // just before the outgoing loop wraps
    engine.deck(1).seekSec(3.0);
    engine.deck(1).play();
    CHECK(player.arm(file, 0, true, &error));
    spinEvents(25);
    CHECK(error.isEmpty());
    CHECK(!engine.deck(1).playing.load());
    // A simulated loop wrap must not rewind the transition's monotonic clock.
    engine.deck(0).seekSec(0.0);
    spinEvents(60);
    CHECK(engine.deck(1).playing.load());
    CHECK(std::fabs(engine.deck(1).positionSec() - 1.0) < 1.0e-6);
    player.abort();

    // PLAY while a recorded hot cue is still held must latch the preview at
    // its advanced position. Re-seeking the incoming anchor here makes the
    // replayed song start several beats earlier than the recorded take.
    TrackDataPtr heldCueTrack = engine.deck(1).track();
    heldCueTrack->hotCues[0] = 0.5;
    engine.deck(0).stop();
    engine.deck(0).seekSec(0.0);
    engine.deck(0).play();
    engine.deck(1).stop();
    GvtFile heldCueReplay;
    heldCueReplay.anchorFromBeat = 0.0;
    heldCueReplay.anchorToBeat = 1.0;
    heldCueReplay.masterBpm = 120.0;
    heldCueReplay.events = {
        {0.0, Role::ToDeck, ControlId::HotCue1, 1.0, Curve::Step},
        {1.0, Role::ToDeck, ControlId::Play, 1.0, Curve::Step},
        {2.0, Role::ToDeck, ControlId::HotCue1, 0.0, Curve::Step},
    };
    CHECK(player.arm(heldCueReplay, 0, true, &error));
    spinEvents();
    CHECK(engine.deck(1).previewActive());
    float replayScratch[24000 * 2] {};
    engine.renderOffline(replayScratch, 24000); // advance both decks by 1 beat
    spinEvents();
    CHECK(engine.deck(1).playing.load());
    CHECK(!engine.deck(1).previewActive());
    CHECK(std::fabs(engine.deck(1).positionSec() - 1.0) < 1.0e-6);
    player.abort();

    // If the incoming deck was already rolling in the recorded pre-state,
    // restore and start it at transition beat zero without requiring a PLAY
    // event or any manual deck alignment.
    file.events.clear();
    file.initialTo.playing = true;
    file.initialTo.positionBeat = 3.0; // 1.5 seconds
    engine.deck(1).stop();
    engine.deck(1).seekSec(0.0);
    CHECK(player.arm(file, 0, true, &error));
    spinEvents();
    CHECK(engine.deck(1).playing.load());
    CHECK(std::fabs(engine.deck(1).positionSec() - 1.5) < 1.0e-6);
    player.abort();

    // Regridding can change native BPM after a transition was recorded. Both
    // initial and later tempo ratios are rebased to preserve effective BPM,
    // while the captured Quantize state is restored for deterministic loops.
    file.initialTo.playing = false;
    file.initialTo.quantizeCaptured = true;
    file.initialTo.quantize = false;
    file.to.bpm = 129.912345;
    file.initialTo.tempoRatio = 0.985643;
    constexpr double eventRatio = 0.986127;
    file.events = {
        {0.0, Role::ToDeck, ControlId::Tempo, eventRatio, Curve::Step},
    };
    engine.deck(1).stop();
    engine.deck(1).quantizeHotCues.store(true);
    CHECK(player.arm(file, 0, true, &error));
    spinEvents();
    const double wantedRatio = file.to.bpm * eventRatio /
                               engine.deck(1).track()->bpm;
    CHECK(std::fabs(engine.deck(1).tempoRatio.load() - wantedRatio) < 1.0e-9);
    CHECK(!engine.deck(1).quantizeHotCues.load());
    player.abort();

    // PRIME must never snap the live/outgoing master deck back to an exact
    // recorded BPM. The DJ has already brought it within tolerance. Incoming
    // setup still restores normally, and later explicit tempo moves remain
    // replayable.
    GvtFile primed = file;
    primed.events.clear();
    primed.masterBpm = 120.0;
    primed.initialFrom.tempoRatio = 1.0;
    primed.initialTo.tempoRatio = 0.9;
    engine.deck(0).tempoRatio.store(1.0025);
    engine.deck(1).tempoRatio.store(1.0);
    transitionPlayerSetMode(&player, PlayerMode::Perform);
    transitionPlayerPreserveOutgoingSetupTempo(&player, true);
    CHECK(player.arm(primed, 0, true, &error));
    spinEvents();
    CHECK(std::fabs(engine.deck(0).tempoRatio.load() - 1.0025) < 1.0e-9);
    const double primedIncomingRatio =
        primed.to.bpm * primed.initialTo.tempoRatio /
        engine.deck(1).track()->bpm;
    CHECK(std::fabs(engine.deck(1).tempoRatio.load() - primedIncomingRatio) <
          1.0e-9);
    player.abort();
    transitionPlayerPreserveOutgoingSetupTempo(&player, false);

    // A recorded CUSTOM loop follows the same press -> PLAY -> release order
    // as the live pad. PLAY takes ownership of the momentary preview, so the
    // recorded release cannot stop or rewind the incoming deck.
    TrackDataPtr customTrack = engine.deck(1).track();
    customTrack->savedLoops[0].startSec = 0.5;
    customTrack->savedLoops[0].endSec = 1.5;
    customTrack->savedLoops[0].label = QStringLiteral("Intro");
    engine.deck(0).loopActive.store(false);
    engine.deck(0).seekSec(0.0);
    engine.deck(0).play();
    GvtFile customReplay;
    customReplay.anchorFromBeat = 0.0;
    customReplay.anchorToBeat = 1.0;
    customReplay.masterBpm = 120.0;
    customReplay.events = {
        {0.0, Role::ToDeck, ControlId::SavedLoop1, 1.0, Curve::Step},
        {0.0, Role::ToDeck, ControlId::Play, 1.0, Curve::Step},
        {0.0, Role::ToDeck, ControlId::SavedLoop1, 0.0, Curve::Step},
    };
    transitionPlayerSetMode(&player, PlayerMode::Perform);
    CHECK(player.arm(customReplay, 0, true, &error));
    spinEvents();
    CHECK(engine.deck(1).playing.load());
    CHECK(engine.deck(1).loopActive.load());
    CHECK(!engine.deck(1).previewActive());
    CHECK(std::fabs(engine.deck(1).positionSec() - 0.5) < 1.0e-6);
    player.abort();

    // Recorder output retains the controller/audio engine's six-decimal
    // tempo precision instead of rounding it to a drift-inducing 0.001.
    engine.deck(0).loadTrack(makeTrack(127.987654));
    engine.deck(0).tempoRatio.store(1.000027);
    engine.deck(0).stop();
    TransitionRecorder recorder(&bus, &engine);
    recorder.start(0);
    constexpr double capturedRatio = 0.985643;
    bus.dispatch({0, ControlId::Tempo, capturedRatio}, Origin::Ui);
    bus.dispatch({0, ControlId::Trim, 0.91}, Origin::Ui);
    bus.dispatch({0, ControlId::TempoRange, 0.50}, Origin::Ui);
    const GvtFile recorded = recorder.finish();
    CHECK(std::fabs(recorded.from.bpm - 127.987654) < 1.0e-9);
    CHECK(std::fabs(recorded.initialFrom.tempoRatio - 1.000027) < 1.0e-9);
    CHECK(recorded.initialFrom.quantizeCaptured);
    CHECK(!recorded.initialFrom.trimCaptured);
    bool foundTempo = false;
    for (const GvtEvent& event : recorded.events) {
        if (event.role == Role::FromDeck &&
            event.control == ControlId::Tempo) {
            foundTempo = true;
            CHECK(std::fabs(event.value - capturedRatio) < 1.0e-9);
        }
    }
    CHECK(foundTempo);
    CHECK(std::none_of(recorded.events.begin(), recorded.events.end(),
                       [](const GvtEvent& event) {
                           return event.control == ControlId::Trim;
                       }));
    CHECK(std::none_of(recorded.events.begin(), recorded.events.end(),
                       [](const GvtEvent& event) {
                           return event.control == ControlId::TempoRange;
                       }));

    // A host performance-pad event is retained as the physical gesture for the
    // audible action that follows it, while remaining absent from replay.
    TransitionRecorder gestureRecorder(&bus, &engine);
    gestureRecorder.start(0);
    bus.dispatch({1, ControlId::PerformancePad4, 3.0}, Origin::Ui);
    bus.dispatch({1, ControlId::Play, 1.0}, Origin::Ui);
    const GvtFile gestureRecorded = gestureRecorder.finish();
    CHECK(gestureRecorded.events.size() == 1);
    if (gestureRecorded.events.size() == 1) {
        CHECK(gestureRecorded.events[0].control == ControlId::Play);
        CHECK(gestureRecorded.events[0].gestureControl ==
              ControlId::PerformancePad4);
        CHECK(gestureRecorded.events[0].gesturePadMode == 3);
    }

    // Recorder timestamps also stay monotonic when the outgoing position
    // wraps from the end of an active loop back to its start.
    Deck& outgoing = engine.deck(0);
    const TrackDataPtr outgoingTrack = outgoing.track();
    outgoing.loopStartSec.store(outgoingTrack->secAtBeat(0.0));
    outgoing.loopEndSec.store(outgoingTrack->secAtBeat(2.0));
    outgoing.loopActive.store(true);
    outgoing.seekSec(outgoingTrack->secAtBeat(1.99));
    outgoing.play();
    TransitionRecorder loopRecorder(&bus, &engine);
    loopRecorder.start(0);
    bus.dispatch({0, ControlId::Fader, 0.4}, Origin::Ui);
    outgoing.seekSec(outgoingTrack->secAtBeat(0.01));
    bus.dispatch({0, ControlId::EqLow, 0.4}, Origin::Ui);
    const GvtFile loopRecorded = loopRecorder.finish();
    CHECK(loopRecorded.events.size() == 2);
    if (loopRecorded.events.size() == 2)
        CHECK(loopRecorded.events[1].beat > loopRecorded.events[0].beat);

    // Tutorial continuous controls remain pending after a mere nudge and are
    // completed only once the student reaches the recorded target.
    GvtFile tutorial;
    tutorial.anchorFromBeat = outgoing.beatPosition();
    tutorial.masterBpm = outgoing.effectiveBpm();
    tutorial.events.push_back(
        {0.0, Role::FromDeck, ControlId::EqHigh, 0.8, Curve::Step});
    int tutorialScores = 0;
    QObject::connect(&player, &TransitionPlayer::tutorialScored,
                     [&tutorialScores](const GvtEvent&, double, double) {
                         ++tutorialScores;
                     });
    transitionPlayerSetMode(&player, PlayerMode::Tutorial);
    CHECK(player.arm(tutorial, 0, true, &error));
    spinEvents();
    bus.dispatch({0, ControlId::EqHigh, 0.3}, Origin::Ui);
    CHECK(tutorialScores == 0);
    bus.dispatch({0, ControlId::EqHigh, 0.8}, Origin::Ui);
    CHECK(tutorialScores == 1);
    player.abort();

    // An action at transition beat zero is announced eight beats before the
    // anchor when the outgoing track has enough runway.
    outgoing.loopActive.store(false);
    outgoing.seekSec(outgoingTrack->secAtBeat(0.0));
    outgoing.play();
    GvtFile countdown;
    countdown.anchorFromBeat = 8.0;
    countdown.masterBpm = outgoing.effectiveBpm();
    countdown.events.push_back(
        {0.0, Role::FromDeck, ControlId::Play, 1.0, Curve::Step});
    int countdownPrompts = 0;
    double countdownLead = 0.0;
    QObject::connect(&player, &TransitionPlayer::tutorialPrompt,
                     [&countdownPrompts, &countdownLead](const GvtEvent&,
                                                        double beatsAhead) {
                         ++countdownPrompts;
                         countdownLead = beatsAhead;
                     });
    transitionPlayerSetMode(&player, PlayerMode::Tutorial);
    CHECK(player.arm(countdown, 0, false, &error));
    spinEvents();
    CHECK(countdownPrompts == 1);
    CHECK(countdownLead > 7.9 && countdownLead <= 8.01);
    player.abort();

    if (failures != 0)
        return 1;
    std::printf("test_transition_replay: loops, tempo, and transport are deterministic\n");
    return 0;
}

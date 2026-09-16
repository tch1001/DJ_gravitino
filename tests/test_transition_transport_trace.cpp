// Protects the editor's derived source-beat timeline, especially loop
// unrolling and semantic transition-cue transport.
#include "transitions/TransitionTransportTrace.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace {
int failures = 0;
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,             \
                   #condition);                                                \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

gvt::TrackDataPtr track(double bpm = 120.0) {
  auto result = std::make_shared<gvt::TrackData>();
  result->bpm = bpm;
  result->durationSec = 300.0;
  return result;
}

gvt::GvtFile baseTransition() {
  gvt::GvtFile file;
  file.sourceFormat = gvt::TransitionSourceFormat::PortableYaml;
  file.masterBpm = 120.0;
  file.from.bpm = 120.0;
  file.to.bpm = 120.0;
  file.anchorFromBeat = 0.0;
  file.anchorToBeat = 16.0;
  file.initialComplete = true;
  file.initialFrom.captured = true;
  file.initialFrom.playing = true;
  file.initialFrom.positionBeat = 0.0;
  file.initialFrom.cueBeat = 0.0;
  file.initialFrom.tempoRatio = 1.0;
  file.initialTo.captured = true;
  file.initialTo.playing = false;
  file.initialTo.positionBeat = 16.0;
  file.initialTo.cueBeat = 16.0;
  file.initialTo.tempoRatio = 1.0;
  return file;
}

bool near(double left, double right) { return std::fabs(left - right) < 0.02; }
} // namespace

int main() {
  {
    gvt::GvtFile file = baseTransition();
    gvt::TransitionSavedLoop loop;
    loop.id = QStringLiteral("outgoing-loop");
    loop.role = gvt::Role::FromDeck;
    loop.startTrackBeat = 4.0;
    loop.endTrackBeat = 6.0;
    loop.preferredPad = 0;
    file.transitionLoops.push_back(loop);

    gvt::GvtEvent engage;
    engage.beat = 4.0;
    engage.role = gvt::Role::FromDeck;
    engage.control = gvt::ControlId::TransitionCue1;
    engage.value = 1.0;
    engage.loopId = loop.id;
    gvt::GvtEvent exit;
    exit.beat = 9.0;
    exit.role = gvt::Role::FromDeck;
    exit.control = gvt::ControlId::LoopExit;
    exit.value = 1.0;
    file.events = {engage, exit};

    const auto trace =
        gvt::buildTransitionTransportTrace(file, track(), track(), 10.0);
    CHECK(trace.usesLoop(gvt::Role::FromDeck, loop.id));
    const auto first = trace.positionAt(gvt::Role::FromDeck, 5.0);
    const auto second = trace.positionAt(gvt::Role::FromDeck, 7.0);
    const auto third = trace.positionAt(gvt::Role::FromDeck, 8.5);
    const auto after = trace.positionAt(gvt::Role::FromDeck, 9.5);
    CHECK(first && first->audible && first->loopPass == 1 &&
          near(first->sourceBeat, 5.0));
    CHECK(second && second->audible && second->loopPass == 2 &&
          near(second->sourceBeat, 5.0));
    CHECK(third && third->audible && third->loopPass == 3 &&
          near(third->sourceBeat, 4.5));
    CHECK(after && after->audible && after->loopId.isEmpty() &&
          near(after->sourceBeat, 5.5));
    const auto repeated =
        trace.transitionBeatsForSource(gvt::Role::FromDeck, 5.0);
    CHECK(repeated.size() >= 3);
  }

  {
    gvt::GvtFile file = baseTransition();
    gvt::TransitionHotCue cue;
    cue.id = QStringLiteral("incoming-entry");
    cue.role = gvt::Role::ToDeck;
    cue.trackBeat = 32.0;
    cue.preferredPad = 0;
    file.transitionCues.push_back(cue);

    gvt::GvtEvent press;
    press.beat = 1.0;
    press.role = gvt::Role::ToDeck;
    press.control = gvt::ControlId::TransitionCue1;
    press.value = 1.0;
    press.cueId = cue.id;
    gvt::GvtEvent play{1.5, gvt::Role::ToDeck, gvt::ControlId::Play, 1.0,
                       gvt::Curve::Step};
    gvt::GvtEvent release = press;
    release.beat = 1.75;
    release.value = 0.0;
    file.events = {press, play, release};

    const auto trace =
        gvt::buildTransitionTransportTrace(file, track(), track(), 3.0);
    const auto before = trace.positionAt(gvt::Role::ToDeck, 0.5);
    const auto preview = trace.positionAt(gvt::Role::ToDeck, 1.25);
    const auto latched = trace.positionAt(gvt::Role::ToDeck, 2.0);
    CHECK(before && !before->audible && near(before->sourceBeat, 16.0));
    CHECK(preview && preview->audible && near(preview->sourceBeat, 32.25));
    CHECK(latched && latched->audible && near(latched->sourceBeat, 33.0));
  }

  {
    // A corrected local BPM changes the engine ratio, not canonical
    // source coordinates or incoming/outgoing alignment.
    gvt::GvtFile file = baseTransition();
    file.to.bpm = 100.0;
    file.initialTo.tempoRatio = 1.2; // authored effective 120 BPM
    file.events = {
        {2.0, gvt::Role::ToDeck, gvt::ControlId::Play, 1.0, gvt::Curve::Step}};
    const auto trace = gvt::buildTransitionTransportTrace(file, track(120.0),
                                                          track(125.0), 4.0);
    const auto started = trace.positionAt(gvt::Role::ToDeck, 3.0);
    CHECK(started && started->audible && near(started->sourceBeat, 17.0));
  }

  {
    gvt::GvtFile file = baseTransition();
    file.events = {
        {2.0, gvt::Role::FromDeck, gvt::ControlId::LoopIn, 1.0,
         gvt::Curve::Step},
        {4.0, gvt::Role::FromDeck, gvt::ControlId::LoopOut, 1.0,
         gvt::Curve::Step},
        {5.0, gvt::Role::FromDeck, gvt::ControlId::LoopHalve, 1.0,
         gvt::Curve::Step},
        {7.0, gvt::Role::FromDeck, gvt::ControlId::LoopExit, 1.0,
         gvt::Curve::Step},
    };
    gvt::TransitionSavedLoop unused;
    unused.id = QStringLiteral("unused-loop");
    unused.role = gvt::Role::FromDeck;
    unused.startTrackBeat = 20.0;
    unused.endTrackBeat = 24.0;
    file.transitionLoops.push_back(unused);
    const auto trace =
        gvt::buildTransitionTransportTrace(file, track(), track(), 8.0);
    const auto firstRepeat = trace.positionAt(gvt::Role::FromDeck, 4.5);
    const auto shortenedRepeat = trace.positionAt(gvt::Role::FromDeck, 6.5);
    const auto afterExit = trace.positionAt(gvt::Role::FromDeck, 7.5);
    CHECK(firstRepeat && firstRepeat->loopId == QStringLiteral("manual-loop") &&
          near(firstRepeat->sourceBeat, 2.5));
    CHECK(shortenedRepeat && shortenedRepeat->loopPass >= 3 &&
          near(shortenedRepeat->sourceBeat, 2.5));
    CHECK(afterExit && afterExit->loopId.isEmpty() &&
          near(afterExit->sourceBeat, 3.5));
    CHECK(!trace.usesLoop(gvt::Role::FromDeck, unused.id));
  }

  {
    gvt::GvtFile file = baseTransition();
    file.initialFrom.positionBeat = -2.0;
    file.anchorFromBeat = -2.0;
    file.initialFrom.loopActive = true;
    file.initialFrom.loopStartBeat = -2.0;
    file.initialFrom.loopEndBeat = 0.0;
    const auto initialTrace =
        gvt::buildTransitionTransportTrace(file, track(), track(), 5.0);
    const auto repeated = initialTrace.positionAt(gvt::Role::FromDeck, 3.0);
    CHECK(repeated && repeated->audible && repeated->loopPass == 2 &&
          near(repeated->sourceBeat, -1.0));

    file.initialFrom.loopActive = false;
    file.initialFrom.positionBeat = 0.0;
    file.anchorFromBeat = 0.0;
    file.events = {
        {1.0, gvt::Role::FromDeck, gvt::ControlId::LoopAuto, 2.0,
         gvt::Curve::Step},
        {4.0, gvt::Role::FromDeck, gvt::ControlId::LoopDouble, 1.0,
         gvt::Curve::Step},
    };
    const auto autoTrace =
        gvt::buildTransitionTransportTrace(file, track(), track(), 6.0);
    const auto autoRepeat = autoTrace.positionAt(gvt::Role::FromDeck, 3.5);
    const auto doubled = autoTrace.positionAt(gvt::Role::FromDeck, 5.5);
    CHECK(autoRepeat && autoRepeat->loopId == QStringLiteral("auto-loop") &&
          autoRepeat->loopPass == 2);
    CHECK(doubled && doubled->loopId == QStringLiteral("auto-loop") &&
          near(doubled->sourceBeat, 3.5));
  }

  if (failures)
    return 1;
  std::printf("test_transition_transport_trace: loop projection passed\n");
  return 0;
}

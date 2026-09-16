// INTERNAL transition transport projection used to draw the editor timeline.
// It converts the authored master-beat clock into piecewise canonical track
// positions without changing or serializing the transition document.
#pragma once

#include "../analysis/TrackData.h"
#include "Transition.h"

#include <array>
#include <optional>
#include <vector>

namespace gvt {

struct TransitionTransportSpan {
  Role role = Role::FromDeck;
  double transitionStartBeat = 0.0;
  double transitionEndBeat = 0.0;
  double sourceStartBeat = 0.0;
  double sourceEndBeat = 0.0;
  bool audible = false;
  QString loopId;
  int loopPass = 0;
};

struct TransitionTransportPosition {
  double sourceBeat = 0.0;
  bool audible = false;
  QString loopId;
  int loopPass = 0;
};

struct TransitionTransportTrace {
  std::array<std::vector<TransitionTransportSpan>, 2> decks;

  const std::vector<TransitionTransportSpan> &spans(Role role) const;
  std::optional<TransitionTransportPosition>
  positionAt(Role role, double transitionBeat) const;
  std::vector<double> transitionBeatsForSource(Role role,
                                               double sourceBeat) const;
  bool usesLoop(Role role, const QString &loopId) const;
};

// Resolve semantic transition cue/loop IDs to their temporary CUSTOM slots,
// discard compatibility-only events, and compensate tempo values for a local
// asset whose beat grid differs from the authored source.
bool resolveTransitionPlaybackEvents(const GvtFile &file,
                                     const TrackDataPtr &outgoing,
                                     const TrackDataPtr &incoming,
                                     std::vector<GvtEvent> &events,
                                     QString *error = nullptr);

TransitionTransportTrace
buildTransitionTransportTrace(const GvtFile &file, const TrackDataPtr &outgoing,
                              const TrackDataPtr &incoming, double endBeat);

} // namespace gvt

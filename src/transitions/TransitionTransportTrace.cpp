// Piecewise transport simulator for the transition editor. The audio engine
// remains authoritative; this deliberately mirrors its deck transport rules
// so looped source audio can be laid out on the monotonic transition clock.
#include "TransitionTransportTrace.h"

#include "PlayerMath.h"
#include "../performance/PerformancePads.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gvt {
namespace {

constexpr double kTraceStepBeats = 1.0 / 64.0;
constexpr double kEpsilon = 1.0e-8;

int roleIndex(Role role) { return role == Role::ToDeck ? 1 : 0; }

const GvtTrackRef &trackRef(const GvtFile &file, Role role) {
  return role == Role::ToDeck ? file.to : file.from;
}

struct DeckProjection {
  Role role = Role::FromDeck;
  double position = 0.0;
  double cue = 0.0;
  double tempoRatio = 1.0;
  double nativeBpm = 0.0;
  bool playing = false;
  bool quantize = true;
  bool loopActive = false;
  double loopStart = 0.0;
  double loopEnd = 0.0;
  QString loopId;
  int loopPass = 0;
  bool loopInPending = false;
  double pendingLoopIn = 0.0;
  ControlId previewControl = ControlId::Count;
  int previewSlot = -1;
  double previewReturn = 0.0;
};

bool sameSpanRun(const TransitionTransportSpan &left,
                 const TransitionTransportSpan &right) {
  if (left.role != right.role || left.audible != right.audible ||
      left.loopId != right.loopId || left.loopPass != right.loopPass ||
      std::fabs(left.transitionEndBeat - right.transitionStartBeat) > kEpsilon)
    return false;
  if (!left.audible)
    return std::fabs(left.sourceEndBeat - right.sourceStartBeat) < kEpsilon;
  const double leftDuration = left.transitionEndBeat - left.transitionStartBeat;
  const double rightDuration =
      right.transitionEndBeat - right.transitionStartBeat;
  if (leftDuration <= 0.0 || rightDuration <= 0.0)
    return false;
  const double leftRate =
      (left.sourceEndBeat - left.sourceStartBeat) / leftDuration;
  const double rightRate =
      (right.sourceEndBeat - right.sourceStartBeat) / rightDuration;
  return std::fabs(left.sourceEndBeat - right.sourceStartBeat) < 1.0e-6 &&
         std::fabs(leftRate - rightRate) < 1.0e-5;
}

void appendSpan(std::vector<TransitionTransportSpan> &spans,
                const TransitionTransportSpan &span) {
  if (!(span.transitionEndBeat > span.transitionStartBeat))
    return;
  if (!spans.empty() && sameSpanRun(spans.back(), span)) {
    spans.back().transitionEndBeat = span.transitionEndBeat;
    spans.back().sourceEndBeat = span.sourceEndBeat;
    return;
  }
  spans.push_back(span);
}

double effectiveBpm(const GvtFile &file, const DeckProjection &deck) {
  const double native =
      deck.nativeBpm > 0.0 ? deck.nativeBpm : trackRef(file, deck.role).bpm;
  if (native > 0.0 && deck.tempoRatio > 0.0)
    return native * deck.tempoRatio;
  return file.masterBpm > 0.0 ? file.masterBpm : 120.0;
}

double tempoValueAt(const std::vector<ScheduledEvent> &schedule, Role role,
                    double beat, double initial) {
  double value = initial;
  for (const ScheduledEvent &scheduled : schedule) {
    if (scheduled.e.role != role || scheduled.e.control != ControlId::Tempo)
      continue;
    if (beat + kEpsilon >= scheduled.e.beat) {
      value = scheduled.e.value;
      continue;
    }
    if (scheduledIsGlide(scheduled) && beat > scheduled.startBeat + kEpsilon)
      return glideValueAt(scheduled, beat);
    break;
  }
  return value;
}

void exitLoopForSeek(DeckProjection &deck, double destination) {
  if (deck.loopActive &&
      (destination < deck.loopStart || destination >= deck.loopEnd))
    deck.loopActive = false;
  deck.position = destination;
}

void activateLoop(DeckProjection &deck, double start, double end,
                  const QString &id, bool seekIfOutside) {
  if (!(end > start))
    return;
  deck.loopStart = start;
  deck.loopEnd = end;
  deck.loopId = id;
  deck.loopPass = 1;
  deck.loopActive = true;
  deck.loopInPending = false;
  if (seekIfOutside && (deck.position < start || deck.position >= end))
    deck.position = start;
}

void advanceDeck(std::vector<TransitionTransportSpan> &output,
                 DeckProjection &deck, double fromBeat, double toBeat,
                 double sourceRate) {
  double cursor = fromBeat;
  if (!deck.playing || !(sourceRate > 0.0)) {
    appendSpan(output, {deck.role,
                        fromBeat,
                        toBeat,
                        deck.position,
                        deck.position,
                        false,
                        {},
                        0});
    return;
  }

  while (cursor + kEpsilon < toBeat) {
    if (deck.loopActive && deck.position >= deck.loopEnd - kEpsilon) {
      const double length = deck.loopEnd - deck.loopStart;
      if (length > 0.0) {
        deck.position =
            deck.loopStart +
            std::fmod(std::max(0.0, deck.position - deck.loopStart), length);
        ++deck.loopPass;
      } else {
        deck.loopActive = false;
      }
    }

    double boundary = std::numeric_limits<double>::infinity();
    QString shownLoop;
    int shownPass = 0;
    if (deck.loopActive) {
      if (deck.position < deck.loopStart - kEpsilon)
        boundary = deck.loopStart;
      else if (deck.position < deck.loopEnd - kEpsilon) {
        boundary = deck.loopEnd;
        shownLoop = deck.loopId;
        shownPass = std::max(1, deck.loopPass);
      }
    }

    const double remainingSource = (toBeat - cursor) * sourceRate;
    double consumedSource = remainingSource;
    if (std::isfinite(boundary))
      consumedSource =
          std::min(consumedSource, std::max(0.0, boundary - deck.position));
    if (consumedSource <= kEpsilon) {
      if (deck.loopActive && boundary == deck.loopEnd) {
        deck.position = deck.loopStart;
        ++deck.loopPass;
        continue;
      }
      consumedSource = remainingSource;
    }

    const double consumedTimeline = consumedSource / sourceRate;
    const double nextCursor = std::min(toBeat, cursor + consumedTimeline);
    const double nextPosition =
        deck.position + (nextCursor - cursor) * sourceRate;
    appendSpan(output, {deck.role, cursor, nextCursor, deck.position,
                        nextPosition, true, shownLoop, shownPass});
    deck.position = nextPosition;
    cursor = nextCursor;
  }
}

const TransitionPerformanceSlot *
slotForControl(const std::array<TransitionPerformanceSlot, 8> &allocations,
               ControlId control) {
  if (control < ControlId::TransitionCue1 ||
      control > ControlId::TransitionCue8)
    return nullptr;
  const int index =
      static_cast<int>(control) - static_cast<int>(ControlId::TransitionCue1);
  return &allocations[static_cast<std::size_t>(index)];
}

void pressMappedCue(DeckProjection &deck, ControlId control, int slot,
                    double cueBeat) {
  if (!std::isfinite(cueBeat))
    return;
  deck.previewControl = control;
  deck.previewSlot = slot;
  deck.previewReturn = cueBeat;
  exitLoopForSeek(deck, cueBeat);
  deck.playing = true;
}

void releasePreview(DeckProjection &deck, ControlId control, int slot) {
  if (deck.previewControl != control || deck.previewSlot != slot)
    return;
  deck.previewControl = ControlId::Count;
  deck.previewSlot = -1;
  deck.playing = false;
  exitLoopForSeek(deck, deck.previewReturn);
}

void applyEvent(const GvtFile &file, DeckProjection &deck,
                const DeckProjection &other, const GvtEvent &event,
                const std::array<TransitionPerformanceSlot, 8> &allocations) {
  const bool pressed = event.value >= 0.5;
  switch (event.control) {
  case ControlId::Play:
    if (!pressed)
      break;
    if (deck.role == Role::ToDeck && deck.previewControl == ControlId::Count)
      exitLoopForSeek(deck, file.anchorToBeat);
    deck.previewControl = ControlId::Count;
    deck.previewSlot = -1;
    deck.playing = true;
    break;
  case ControlId::Stop:
    if (pressed)
      deck.playing = false;
    break;
  case ControlId::Cue:
    if (!pressed) {
      releasePreview(deck, ControlId::Cue, -1);
    } else if (deck.playing) {
      deck.previewControl = ControlId::Count;
      deck.playing = false;
      exitLoopForSeek(deck, deck.cue);
    } else if (std::fabs(deck.position - deck.cue) <= 0.02) {
      deck.previewControl = ControlId::Cue;
      deck.previewSlot = -1;
      deck.previewReturn = deck.cue;
      deck.playing = true;
    } else {
      deck.cue = deck.position;
    }
    break;
  case ControlId::HotCue1:
  case ControlId::HotCue2:
  case ControlId::HotCue3:
  case ControlId::HotCue4:
  case ControlId::HotCue5:
  case ControlId::HotCue6:
  case ControlId::HotCue7:
  case ControlId::HotCue8: {
    const int pad =
        static_cast<int>(event.control) - static_cast<int>(ControlId::HotCue1);
    if (!pressed) {
      releasePreview(deck, event.control, pad);
      break;
    }
    const auto &mappings =
        deck.role == Role::FromDeck ? file.fromHotCueBeats : file.toHotCueBeats;
    if (hotCueBeatIsMapped(mappings[static_cast<std::size_t>(pad)]))
      pressMappedCue(deck, event.control, pad,
                     mappings[static_cast<std::size_t>(pad)]);
    break;
  }
  case ControlId::TransitionCue1:
  case ControlId::TransitionCue2:
  case ControlId::TransitionCue3:
  case ControlId::TransitionCue4:
  case ControlId::TransitionCue5:
  case ControlId::TransitionCue6:
  case ControlId::TransitionCue7:
  case ControlId::TransitionCue8: {
    const int pad = static_cast<int>(event.control) -
                    static_cast<int>(ControlId::TransitionCue1);
    const TransitionPerformanceSlot *allocation =
        slotForControl(allocations, event.control);
    if (!allocation)
      break;
    if (!pressed) {
      releasePreview(deck, event.control, pad);
    } else if (allocation->cue) {
      pressMappedCue(deck, event.control, pad, allocation->cue->trackBeat);
    } else if (allocation->loop) {
      const bool ordinaryPlayback =
          deck.playing && deck.previewControl == ControlId::Count;
      if (ordinaryPlayback) {
        if (deck.position < allocation->loop->endTrackBeat)
          activateLoop(deck, allocation->loop->startTrackBeat,
                       allocation->loop->endTrackBeat, allocation->loop->id,
                       false);
        else
          deck.loopActive = false;
      } else {
        activateLoop(deck, allocation->loop->startTrackBeat,
                     allocation->loop->endTrackBeat, allocation->loop->id,
                     true);
        deck.position = allocation->loop->startTrackBeat;
        deck.previewControl = event.control;
        deck.previewSlot = pad;
        deck.previewReturn = allocation->loop->startTrackBeat;
        deck.playing = true;
      }
    }
    break;
  }
  case ControlId::LoopAuto:
    if (event.value > 0.0) {
      const double start = std::floor(deck.position);
      activateLoop(deck, start, start + event.value,
                   QStringLiteral("auto-loop"), false);
    }
    break;
  case ControlId::LoopIn:
    if (pressed) {
      deck.loopActive = false;
      deck.pendingLoopIn =
          deck.quantize ? std::round(deck.position) : deck.position;
      deck.loopInPending = true;
    }
    break;
  case ControlId::LoopOut:
    if (pressed && deck.loopInPending) {
      double end = deck.quantize ? std::round(deck.position) : deck.position;
      if (end <= deck.pendingLoopIn && deck.quantize)
        end = deck.pendingLoopIn + 1.0;
      activateLoop(deck, deck.pendingLoopIn, end, QStringLiteral("manual-loop"),
                   false);
    }
    break;
  case ControlId::LoopExit:
    if (pressed)
      deck.loopActive = false;
    break;
  case ControlId::LoopHalve:
    if (pressed && deck.loopActive)
      deck.loopEnd = deck.loopStart +
                     std::max(0.03125, (deck.loopEnd - deck.loopStart) * 0.5);
    break;
  case ControlId::LoopDouble:
    if (pressed && deck.loopActive)
      deck.loopEnd = deck.loopStart +
                     std::min(64.0, (deck.loopEnd - deck.loopStart) * 2.0);
    break;
  case ControlId::BeatJump:
    exitLoopForSeek(deck, std::round(deck.position) + event.value);
    break;
  case ControlId::TempoSync: {
    const double phase = other.position - std::floor(other.position);
    exitLoopForSeek(deck, std::round(deck.position - phase) + phase);
    break;
  }
  case ControlId::PlatterScratch: {
    const double native = trackRef(file, deck.role).bpm;
    const double delta =
        event.value * 0.01 * (native > 0.0 ? native : 120.0) / 60.0;
    double destination = deck.position + delta;
    if (deck.loopActive)
      destination = std::clamp(destination, deck.loopStart,
                               std::nextafter(deck.loopEnd, deck.loopStart));
    deck.position = destination;
    break;
  }
  case ControlId::Tempo:
    deck.tempoRatio = event.value;
    break;
  case ControlId::Quantize:
    deck.quantize = pressed;
    break;
  default:
    break;
  }
}

DeckProjection initialDeck(const GvtFile &file, Role role) {
  DeckProjection deck;
  deck.role = role;
  const bool outgoing = role == Role::FromDeck;
  const GvtInitialState &state = outgoing ? file.initialFrom : file.initialTo;
  if (file.initialComplete && state.captured) {
    deck.position = outgoing ? file.anchorFromBeat : state.positionBeat;
    deck.cue = state.cueBeat;
    deck.tempoRatio = state.tempoRatio;
    deck.playing = outgoing || state.playing;
    deck.quantize = state.quantizeCaptured ? state.quantize : true;
    deck.loopActive =
        state.loopActive && state.loopEndBeat > state.loopStartBeat;
    deck.loopStart = state.loopStartBeat;
    deck.loopEnd = state.loopEndBeat;
    if (deck.loopActive) {
      deck.loopId = QStringLiteral("initial-loop");
      const auto definition = std::find_if(
          file.transitionLoops.begin(), file.transitionLoops.end(),
          [&deck, role](const TransitionSavedLoop &loop) {
            return loop.role == role &&
                   std::fabs(loop.startTrackBeat - deck.loopStart) < 1.0e-6 &&
                   std::fabs(loop.endTrackBeat - deck.loopEnd) < 1.0e-6;
          });
      if (definition != file.transitionLoops.end())
        deck.loopId = definition->id;
    }
    deck.loopPass = deck.loopActive ? 1 : 0;
    return deck;
  }
  deck.position = outgoing ? file.anchorFromBeat : file.anchorToBeat;
  deck.cue = deck.position;
  deck.playing = outgoing;
  if (state.captured)
    deck.tempoRatio = state.tempoRatio;
  return deck;
}

} // namespace

const std::vector<TransitionTransportSpan> &
TransitionTransportTrace::spans(Role role) const {
  return decks[static_cast<std::size_t>(roleIndex(role))];
}

std::optional<TransitionTransportPosition>
TransitionTransportTrace::positionAt(Role role, double transitionBeat) const {
  const auto &roleSpans = spans(role);
  if (roleSpans.empty())
    return std::nullopt;
  auto found =
      std::upper_bound(roleSpans.begin(), roleSpans.end(), transitionBeat,
                       [](double beat, const TransitionTransportSpan &span) {
                         return beat < span.transitionStartBeat;
                       });
  if (found != roleSpans.begin())
    --found;
  if (transitionBeat < found->transitionStartBeat - kEpsilon ||
      transitionBeat > found->transitionEndBeat + kEpsilon)
    return std::nullopt;
  const double duration = found->transitionEndBeat - found->transitionStartBeat;
  const double fraction =
      duration > 0.0
          ? std::clamp((transitionBeat - found->transitionStartBeat) / duration,
                       0.0, 1.0)
          : 0.0;
  return TransitionTransportPosition{
      found->sourceStartBeat +
          fraction * (found->sourceEndBeat - found->sourceStartBeat),
      found->audible, found->loopId, found->loopPass};
}

std::vector<double>
TransitionTransportTrace::transitionBeatsForSource(Role role,
                                                   double sourceBeat) const {
  std::vector<double> result;
  for (const TransitionTransportSpan &span : spans(role)) {
    if (!span.audible)
      continue;
    const double low = std::min(span.sourceStartBeat, span.sourceEndBeat);
    const double high = std::max(span.sourceStartBeat, span.sourceEndBeat);
    if (sourceBeat < low - kEpsilon || sourceBeat > high + kEpsilon)
      continue;
    const double sourceDuration = span.sourceEndBeat - span.sourceStartBeat;
    if (std::fabs(sourceDuration) <= kEpsilon)
      continue;
    const double fraction =
        (sourceBeat - span.sourceStartBeat) / sourceDuration;
    const double beat =
        span.transitionStartBeat +
        fraction * (span.transitionEndBeat - span.transitionStartBeat);
    if (result.empty() || std::fabs(result.back() - beat) > 1.0e-5)
      result.push_back(beat);
  }
  return result;
}

bool TransitionTransportTrace::usesLoop(Role role,
                                        const QString &loopId) const {
  return std::any_of(spans(role).begin(), spans(role).end(),
                     [&loopId](const TransitionTransportSpan &span) {
                       return !loopId.isEmpty() && span.loopId == loopId;
                     });
}

bool resolveTransitionPlaybackEvents(const GvtFile &file,
                                     const TrackDataPtr &outgoing,
                                     const TrackDataPtr &incoming,
                                     std::vector<GvtEvent> &events,
                                     QString *error) {
  events.clear();
  std::copy_if(file.events.begin(), file.events.end(),
               std::back_inserter(events), [](const GvtEvent& event) {
                 return transitionEventIsExecutable(event) &&
                        event.control != ControlId::Trim;
               });
  const auto outgoingSlots = transitionPerformanceSlots(file, Role::FromDeck);
  const auto incomingSlots = transitionPerformanceSlots(file, Role::ToDeck);
  for (GvtEvent &event : events) {
    if (event.control == ControlId::Tempo) {
      event.value =
          event.role == Role::FromDeck
              ? transitionReplayTempoEvent(event.value, file.from, outgoing)
              : transitionReplayTempoEvent(event.value, file.to, incoming);
    }
    if (event.role == Role::Mixer ||
        (event.cueId.isEmpty() && event.loopId.isEmpty()))
      continue;
    const auto &allocations =
        event.role == Role::FromDeck ? outgoingSlots : incomingSlots;
    const auto found =
        std::find_if(allocations.begin(), allocations.end(),
                     [&event](const auto &allocation) {
                       return (!event.cueId.isEmpty() && allocation.cue &&
                               allocation.cue->id == event.cueId) ||
                              (!event.loopId.isEmpty() && allocation.loop &&
                               allocation.loop->id == event.loopId);
                     });
    if (found == allocations.end()) {
      if (error)
        *error =
            QStringLiteral("timeline refers to unallocated transition pad '%1'")
                .arg(event.cueId.isEmpty() ? event.loopId : event.cueId);
      events.clear();
      return false;
    }
    const int allocationIndex =
        static_cast<int>(std::distance(allocations.begin(), found));
    event.control = static_cast<ControlId>(
        static_cast<int>(ControlId::TransitionCue1) + allocationIndex);
    event.gestureControl = static_cast<ControlId>(
        static_cast<int>(ControlId::PerformancePad1) + allocationIndex);
    event.gesturePadMode = static_cast<int>(PerformancePadMode::Sampler);
  }
  std::stable_sort(events.begin(), events.end(),
                   [](const GvtEvent &left, const GvtEvent &right) {
                     return left.beat < right.beat;
                   });
  return true;
}

TransitionTransportTrace
buildTransitionTransportTrace(const GvtFile &file, const TrackDataPtr &outgoing,
                              const TrackDataPtr &incoming, double endBeat) {
  TransitionTransportTrace trace;
  if (!(endBeat > 0.0))
    return trace;

  std::vector<GvtEvent> events;
  if (!resolveTransitionPlaybackEvents(file, outgoing, incoming, events))
    return trace;
  DeckProjection from = initialDeck(file, Role::FromDeck);
  DeckProjection to = initialDeck(file, Role::ToDeck);
  from.nativeBpm =
      outgoing && outgoing->bpm > 0.0 ? outgoing->bpm : file.from.bpm;
  to.nativeBpm = incoming && incoming->bpm > 0.0 ? incoming->bpm : file.to.bpm;

  // Match Preview::reset: portable/local beat-grid compensation changes the
  // engine ratios, but the resulting effective BPM remains the authored BPM.
  if (outgoing && outgoing->bpm > 0.0) {
    const double wanted =
        file.masterBpm > 0.0 ? file.masterBpm : file.from.bpm * from.tempoRatio;
    if (wanted > 0.0)
      from.tempoRatio = wanted / outgoing->bpm;
  }
  if (incoming && incoming->bpm > 0.0 && file.to.bpm > 0.0)
    to.tempoRatio = file.to.bpm * to.tempoRatio / incoming->bpm;

  const auto outgoingSlots = transitionPerformanceSlots(file, Role::FromDeck);
  const auto incomingSlots = transitionPerformanceSlots(file, Role::ToDeck);
  const auto schedule =
      buildSchedule(events, [&from, &to](Role role, ControlId control) {
        if (control != ControlId::Tempo)
          return 0.0;
        return role == Role::ToDeck ? to.tempoRatio : from.tempoRatio;
      });
  const double initialFromTempo = from.tempoRatio;
  const double initialToTempo = to.tempoRatio;

  std::size_t eventIndex = 0;
  double cursor = 0.0;
  while (cursor < endBeat - kEpsilon) {
    while (eventIndex < events.size() &&
           events[eventIndex].beat <= cursor + kEpsilon) {
      const GvtEvent &event = events[eventIndex++];
      if (event.role == Role::FromDeck)
        applyEvent(file, from, to, event, outgoingSlots);
      else if (event.role == Role::ToDeck)
        applyEvent(file, to, from, event, incomingSlots);
    }

    double next = std::min(endBeat, cursor + kTraceStepBeats);
    if (eventIndex < events.size() &&
        events[eventIndex].beat > cursor + kEpsilon)
      next = std::min(next, events[eventIndex].beat);
    if (!(next > cursor + kEpsilon)) {
      cursor = std::nextafter(cursor, endBeat);
      continue;
    }
    const double midpoint = 0.5 * (cursor + next);
    from.tempoRatio =
        tempoValueAt(schedule, Role::FromDeck, midpoint, initialFromTempo);
    to.tempoRatio =
        tempoValueAt(schedule, Role::ToDeck, midpoint, initialToTempo);
    const double clockBpm = std::max(1.0, effectiveBpm(file, from));
    const double fromRate = effectiveBpm(file, from) / clockBpm;
    const double toRate = effectiveBpm(file, to) / clockBpm;
    advanceDeck(trace.decks[0], from, cursor, next, fromRate);
    advanceDeck(trace.decks[1], to, cursor, next, toRate);
    cursor = next;
  }
  return trace;
}

} // namespace gvt

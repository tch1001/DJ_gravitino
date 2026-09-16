// Shared setup for live Perform/PRIME and isolated editor audition. Keeping
// preparation here prevents the editor from inventing a second interpretation
// of the same saved snapshot. No document or permanent track cue is modified.
#pragma once

#include "Transition.h"
#include "../audio/AudioEngine.h"

namespace gvt {

struct TransitionSetupOptions {
    bool prepareOutgoingTransport = true;
    bool prepareIncomingTransport = true;
    bool applyOutgoingTempo = true;
    bool applyHardwareFacingState = true;
};

void prepareTransitionSetup(ControlBus& bus, AudioEngine& engine,
                            const GvtFile& file, int outgoingDeck,
                            TransitionSetupOptions options = {});

// Perform starts the outgoing song at its anchor, regardless of a historical
// initial-position/playing snapshot. PRIME retains the live outgoing position.
void positionTransitionPerform(AudioEngine& engine, const GvtFile& file,
                               int outgoingDeck);

double transitionEngineValue(AudioEngine& engine, int outgoingDeck,
                             Role role, ControlId control);

// Copy live musical context into an isolated graph, in role space. In
// particular trim, key lock and crossfader are NOT transition-owned, but must
// not silently change the sound when auditioning. Never writes the source.
void copyTransitionPlaybackContext(AudioEngine& source, int sourceOutgoing,
                                   ControlBus& targetBus, AudioEngine& target);

} // namespace gvt

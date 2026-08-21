// INTERNAL header (not pinned) — mode selection for TransitionPlayer.
// The pinned TransitionEngine.h declares PlayerMode but no setter, so the
// mode is set out-of-band via this free function (static side-table inside
// TransitionPlayer.cpp). Call it BEFORE arm(); default is PlayerMode::Perform.
// Owner: claude-transitions. Documented in docs/STATUS.md.
#pragma once
#include "TransitionEngine.h"

namespace gvt {

void transitionPlayerSetMode(TransitionPlayer* player, PlayerMode mode);

// PRIME deliberately leaves the already-playing/outgoing deck's tempo under
// the DJ's control.  Suppress only the captured beat-zero tempo restore for
// that deck; later recorded tempo moves still replay normally.
void transitionPlayerPreserveOutgoingSetupTempo(TransitionPlayer* player,
                                                bool preserve);

} // namespace gvt

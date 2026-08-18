#pragma once

#include "../control/ControlBus.h"

#include <cstdint>
#include <optional>

namespace gvt {

// Logical regions on the DDJ-FLX4 surface used by the visual transition
// tutor. Keeping this table beside the real MIDI mapping makes unsupported
// actions explicit instead of inventing a hardware control in the UI.
enum class Flx4SurfaceControl : std::uint8_t {
    PlayPause,
    Cue,
    Sync,
    Load,
    PerformancePad,
    LoopIn,
    LoopOut,
    FourBeatExit,
    LoopHalve,
    LoopDouble,
    TempoFader,
    JogWheel,
    ChannelFader,
    Trim,
    EqHigh,
    EqMid,
    EqLow,
    Filter,
    Crossfader,
    ChannelCue,
    MasterCue,
    HeadphoneMix,
    BeatFxSelect,
    BeatFxOn,
    BeatFxWet,
    BeatFxBeats,
};

enum class Flx4PadMode : std::uint8_t { None, HotCue, BeatJump };

struct Flx4TutorialMapping {
    Flx4SurfaceControl surface = Flx4SurfaceControl::PlayPause;
    int pad = -1; // zero-based performance-pad index, when applicable
    Flx4PadMode padMode = Flx4PadMode::None;
    bool needsFxAssignment = false;
};

// Returns no value when a recorded action cannot be performed directly on
// the mapped FLX4 surface. Value-sensitive controls (4 BEAT and beat-jump
// pads) are rejected when the requested value has no exact hardware gesture.
std::optional<Flx4TutorialMapping> flx4TutorialMapping(
    ControlId id, double value) noexcept;

} // namespace gvt

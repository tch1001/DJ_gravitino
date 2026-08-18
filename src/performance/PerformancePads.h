#pragma once

#include <string>

namespace gvt {

constexpr int kPerformancePadCount = 8;

enum class PerformancePadMode {
    HotCue = 0,
    PadFx1,
    BeatJump,
    Sampler,
    Keyboard,
    PadFx2,
    BeatLoop,
    KeyShift,
    SavedLoop, // virtual/host layer; no dedicated FLX4 hardware mode button
    Count,
};

enum class PerformancePadAction {
    HotCue,
    FxHold,
    BeatJump,
    BeatLoop,
    SamplerSlot,
    KeyboardNote,
    KeyShift,
    SavedLoop,
};

// UI-independent assignment state. Unsupported actions are still represented
// honestly so their programming survives restarts and can be wired to a future
// sampler/pitch engine without changing the saved settings format.
struct PerformancePadAssignment {
    PerformancePadAction action = PerformancePadAction::HotCue;
    double value = 0.0;       // beats, MIDI note, or key-shift semitones
    int fxType = 0;           // 0 echo, 1 reverb, 2 flanger
    double fxWet = 0.5;
    double fxBeats = 0.5;
    std::string label;
    std::string resource;     // sampler file path (configuration only today)
};

const char* performancePadModeKey(PerformancePadMode mode);
const char* performancePadModeLabel(PerformancePadMode mode);
bool performancePadModeIsShifted(PerformancePadMode mode);
bool performancePadActionIsSupported(PerformancePadAction action);

PerformancePadAssignment defaultPerformancePadAssignment(
    PerformancePadMode mode, int pad);
PerformancePadAssignment sanitizePerformancePadAssignment(
    PerformancePadMode mode, int pad,
    const PerformancePadAssignment& assignment);

} // namespace gvt

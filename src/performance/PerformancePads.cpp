#include "PerformancePads.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace gvt {
namespace {

int boundedPad(int pad)
{
    return std::clamp(pad, 0, kPerformancePadCount - 1);
}

std::string signedLabel(double value)
{
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%+.0f", value);
    return text;
}

std::string beatLabel(double beats)
{
    if (beats < 1.0) {
        const int denominator = std::max(1, (int)std::lround(1.0 / beats));
        return "1/" + std::to_string(denominator);
    }
    return std::to_string((int)std::lround(beats));
}

PerformancePadAction actionForMode(PerformancePadMode mode)
{
    switch (mode) {
    case PerformancePadMode::HotCue:  return PerformancePadAction::HotCue;
    case PerformancePadMode::PadFx1:
    case PerformancePadMode::PadFx2:  return PerformancePadAction::FxHold;
    case PerformancePadMode::BeatJump:return PerformancePadAction::BeatJump;
    case PerformancePadMode::BeatLoop:return PerformancePadAction::BeatLoop;
    case PerformancePadMode::Sampler: return PerformancePadAction::SamplerSlot;
    case PerformancePadMode::Keyboard:return PerformancePadAction::KeyboardNote;
    case PerformancePadMode::KeyShift:return PerformancePadAction::KeyShift;
    case PerformancePadMode::SavedLoop:return PerformancePadAction::SavedLoop;
    case PerformancePadMode::Count:   return PerformancePadAction::HotCue;
    }
    return PerformancePadAction::HotCue;
}

} // namespace

const char* performancePadModeKey(PerformancePadMode mode)
{
    switch (mode) {
    case PerformancePadMode::HotCue:   return "hotCue";
    case PerformancePadMode::PadFx1:    return "padFx1";
    case PerformancePadMode::BeatJump:  return "beatJump";
    case PerformancePadMode::Sampler:   return "sampler";
    case PerformancePadMode::Keyboard:  return "keyboard";
    case PerformancePadMode::PadFx2:    return "padFx2";
    case PerformancePadMode::BeatLoop:  return "beatLoop";
    case PerformancePadMode::KeyShift:  return "keyShift";
    case PerformancePadMode::SavedLoop: return "savedLoop";
    case PerformancePadMode::Count:     return "hotCue";
    }
    return "hotCue";
}

const char* performancePadModeLabel(PerformancePadMode mode)
{
    switch (mode) {
    case PerformancePadMode::HotCue:   return "HOT CUE";
    case PerformancePadMode::PadFx1:    return "PAD FX1";
    case PerformancePadMode::BeatJump:  return "BEAT JUMP";
    case PerformancePadMode::Sampler:   return "CUSTOM";
    case PerformancePadMode::Keyboard:  return "KEYBOARD";
    case PerformancePadMode::PadFx2:    return "PAD FX2";
    case PerformancePadMode::BeatLoop:  return "BEAT LOOP";
    case PerformancePadMode::KeyShift:  return "KEY SHIFT";
    case PerformancePadMode::SavedLoop: return "CUSTOM";
    case PerformancePadMode::Count:     return "HOT CUE";
    }
    return "HOT CUE";
}

bool performancePadModeIsShifted(PerformancePadMode mode)
{
    return mode == PerformancePadMode::Keyboard ||
           mode == PerformancePadMode::PadFx2 ||
           mode == PerformancePadMode::BeatLoop ||
           mode == PerformancePadMode::KeyShift;
}

bool performancePadActionIsSupported(PerformancePadAction action)
{
    return action == PerformancePadAction::HotCue ||
           action == PerformancePadAction::FxHold ||
           action == PerformancePadAction::BeatJump ||
           action == PerformancePadAction::BeatLoop ||
           action == PerformancePadAction::SamplerSlot ||
           action == PerformancePadAction::SavedLoop;
}

PerformancePadAssignment defaultPerformancePadAssignment(
    PerformancePadMode mode, int pad)
{
    const int index = boundedPad(pad);
    PerformancePadAssignment result;
    result.action = actionForMode(mode);

    switch (mode) {
    case PerformancePadMode::HotCue:
        result.value = index;
        result.label = std::to_string(index + 1);
        break;
    case PerformancePadMode::BeatJump: {
        static constexpr std::array<double, 8> beats = {
            -16.0, -8.0, -4.0, -1.0, 1.0, 4.0, 8.0, 16.0};
        result.value = beats[index];
        result.label = signedLabel(result.value);
        break;
    }
    case PerformancePadMode::BeatLoop: {
        static constexpr std::array<double, 8> beats = {
            0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0};
        result.value = beats[index];
        result.label = beatLabel(result.value);
        break;
    }
    case PerformancePadMode::PadFx1:
    case PerformancePadMode::PadFx2: {
        static constexpr std::array<int, 8> types1 = {0, 0, 0, 1, 1, 1, 2, 2};
        static constexpr std::array<int, 8> types2 = {2, 2, 1, 1, 0, 0, 2, 1};
        static constexpr std::array<double, 8> beats1 = {
            0.25, 0.5, 1.0, 0.5, 1.0, 2.0, 0.5, 1.0};
        static constexpr std::array<double, 8> beats2 = {
            0.25, 2.0, 0.25, 4.0, 2.0, 4.0, 1.0, 2.0};
        result.fxType = mode == PerformancePadMode::PadFx1
                            ? types1[index] : types2[index];
        result.fxBeats = mode == PerformancePadMode::PadFx1
                             ? beats1[index] : beats2[index];
        result.fxWet = mode == PerformancePadMode::PadFx1
                           ? (0.35 + 0.08 * (index % 3))
                           : (0.55 + 0.06 * (index % 3));
        static constexpr const char* prefixes[3] = {"E", "R", "F"};
        result.label = std::string(prefixes[result.fxType]) +
                       beatLabel(result.fxBeats);
        break;
    }
    case PerformancePadMode::Sampler:
        result.value = index;
        result.label = "S" + std::to_string(index + 1);
        break;
    case PerformancePadMode::Keyboard: {
        static constexpr std::array<int, 8> notes = {60, 62, 64, 65, 67, 69, 71, 72};
        static constexpr const char* labels[8] = {"C4", "D4", "E4", "F4",
                                                  "G4", "A4", "B4", "C5"};
        result.value = notes[index];
        result.label = labels[index];
        break;
    }
    case PerformancePadMode::KeyShift: {
        static constexpr std::array<double, 8> semitones = {
            -4.0, -2.0, -1.0, 0.0, 1.0, 2.0, 4.0, 7.0};
        result.value = semitones[index];
        result.label = signedLabel(result.value);
        break;
    }
    case PerformancePadMode::SavedLoop:
        result.value = index;
        result.label = "L" + std::to_string(index + 1);
        break;
    case PerformancePadMode::Count:
        break;
    }
    return result;
}

PerformancePadAssignment sanitizePerformancePadAssignment(
    PerformancePadMode mode, int pad,
    const PerformancePadAssignment& assignment)
{
    PerformancePadAssignment result = assignment;
    const PerformancePadAssignment defaults =
        defaultPerformancePadAssignment(mode, pad);
    result.action = defaults.action;

    if (!std::isfinite(result.value)) result.value = defaults.value;
    if (!std::isfinite(result.fxWet)) result.fxWet = defaults.fxWet;
    if (!std::isfinite(result.fxBeats)) result.fxBeats = defaults.fxBeats;

    switch (result.action) {
    case PerformancePadAction::BeatJump:
        result.value = std::clamp(result.value, -64.0, 64.0);
        if (std::abs(result.value) < 0.125) result.value = defaults.value;
        break;
    case PerformancePadAction::BeatLoop:
        result.value = std::clamp(result.value, 0.125, 64.0);
        break;
    case PerformancePadAction::FxHold:
        result.fxType = std::clamp(result.fxType, 0, 2);
        result.fxWet = std::clamp(result.fxWet, 0.0, 1.0);
        result.fxBeats = std::clamp(result.fxBeats, 0.25, 4.0);
        break;
    case PerformancePadAction::KeyboardNote:
        result.value = std::clamp(result.value, 0.0, 127.0);
        break;
    case PerformancePadAction::KeyShift:
        result.value = std::clamp(result.value, -12.0, 12.0);
        break;
    case PerformancePadAction::SavedLoop:
        result.value = std::clamp(result.value, 0.0, 7.0);
        break;
    case PerformancePadAction::HotCue:
    case PerformancePadAction::SamplerSlot:
        break;
    }

    if (result.label.empty()) result.label = defaults.label;
    return result;
}

} // namespace gvt

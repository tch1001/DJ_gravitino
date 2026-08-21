#include "ControlBus.h"
#include <cstring>

namespace gvt {

namespace {
struct NameEntry { ControlId id; const char* name; };
constexpr NameEntry kNames[] = {
    {ControlId::Play, "play"}, {ControlId::Stop, "stop"}, {ControlId::Cue, "cue"},
    {ControlId::Load, "load"}, {ControlId::BrowseSelect, "browse_select"},
    {ControlId::TempoSync, "tempo_sync"},
    {ControlId::HotCue1, "hotcue_1"}, {ControlId::HotCue2, "hotcue_2"},
    {ControlId::HotCue3, "hotcue_3"}, {ControlId::HotCue4, "hotcue_4"},
    {ControlId::HotCue5, "hotcue_5"}, {ControlId::HotCue6, "hotcue_6"},
    {ControlId::HotCue7, "hotcue_7"}, {ControlId::HotCue8, "hotcue_8"},
    {ControlId::BrowseNavigate, "browse_navigate"},
    {ControlId::Tempo, "tempo"}, {ControlId::Fader, "fader"},
    {ControlId::Trim, "trim"}, {ControlId::EqLow, "eq_low"},
    {ControlId::EqMid, "eq_mid"}, {ControlId::EqHigh, "eq_high"},
    {ControlId::Crossfader, "xfader"},
    {ControlId::HeadphoneCue, "headphone_cue"},
    {ControlId::MasterCue, "master_cue"},
    {ControlId::HeadphoneMix, "headphone_mix"},
    {ControlId::Jog, "jog"},
    {ControlId::LoopIn, "loop_in"}, {ControlId::LoopOut, "loop_out"},
    {ControlId::LoopExit, "loop_exit"}, {ControlId::LoopHalve, "loop_halve"},
    {ControlId::LoopDouble, "loop_double"}, {ControlId::LoopAuto, "loop_auto"},
    {ControlId::BeatJump, "beat_jump"}, {ControlId::Filter, "filter"},
    {ControlId::FxType, "fx_type"}, {ControlId::FxOn, "fx_on"},
    {ControlId::FxWet, "fx_wet"}, {ControlId::FxBeats, "fx_beats"},
    {ControlId::StemVocals, "stem_vocals"}, {ControlId::StemMelody, "stem_melody"},
    {ControlId::StemBass, "stem_bass"}, {ControlId::StemDrums, "stem_drums"},
    {ControlId::PlatterScratch, "platter_scratch"},
    {ControlId::Quantize, "quantize"},
    {ControlId::PerformancePadMode, "performance_pad_mode"},
    {ControlId::PerformancePad1, "performance_pad_1"},
    {ControlId::PerformancePad2, "performance_pad_2"},
    {ControlId::PerformancePad3, "performance_pad_3"},
    {ControlId::PerformancePad4, "performance_pad_4"},
    {ControlId::PerformancePad5, "performance_pad_5"},
    {ControlId::PerformancePad6, "performance_pad_6"},
    {ControlId::PerformancePad7, "performance_pad_7"},
    {ControlId::PerformancePad8, "performance_pad_8"},
    {ControlId::PlatterTouch, "platter_touch"},
    {ControlId::SavedLoop1, "saved_loop_1"},
    {ControlId::SavedLoop2, "saved_loop_2"},
    {ControlId::SavedLoop3, "saved_loop_3"},
    {ControlId::SavedLoop4, "saved_loop_4"},
    {ControlId::SavedLoop5, "saved_loop_5"},
    {ControlId::SavedLoop6, "saved_loop_6"},
    {ControlId::SavedLoop7, "saved_loop_7"},
    {ControlId::SavedLoop8, "saved_loop_8"},
};
} // namespace

bool controlIsTrigger(ControlId id) {
    switch (id) {
        case ControlId::Play: case ControlId::Stop: case ControlId::Cue:
        case ControlId::Load: case ControlId::BrowseSelect:
        case ControlId::TempoSync:
        case ControlId::HotCue1: case ControlId::HotCue2: case ControlId::HotCue3:
        case ControlId::HotCue4: case ControlId::HotCue5: case ControlId::HotCue6:
        case ControlId::HotCue7: case ControlId::HotCue8:
        case ControlId::LoopIn: case ControlId::LoopOut:
        case ControlId::LoopExit: case ControlId::LoopHalve:
        case ControlId::LoopDouble:
        case ControlId::PerformancePadMode:
        case ControlId::PerformancePad1: case ControlId::PerformancePad2:
        case ControlId::PerformancePad3: case ControlId::PerformancePad4:
        case ControlId::PerformancePad5: case ControlId::PerformancePad6:
        case ControlId::PerformancePad7: case ControlId::PerformancePad8:
        case ControlId::PlatterTouch:
        case ControlId::SavedLoop1: case ControlId::SavedLoop2:
        case ControlId::SavedLoop3: case ControlId::SavedLoop4:
        case ControlId::SavedLoop5: case ControlId::SavedLoop6:
        case ControlId::SavedLoop7: case ControlId::SavedLoop8:
            return true;
        // LoopAuto/BeatJump carry a beats value; Filter is continuous.
        default:
            return false;
    }
}

const char* controlName(ControlId id) {
    for (const auto& e : kNames)
        if (e.id == id) return e.name;
    return "unknown";
}

bool controlFromName(const char* name, ControlId& out) {
    for (const auto& e : kNames)
        if (std::strcmp(e.name, name) == 0) { out = e.id; return true; }
    return false;
}

ControlBus::ControlBus(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ControlEvent>();
    qRegisterMetaType<Origin>();
}

void ControlBus::dispatch(const ControlEvent& e, Origin origin) {
    emit eventDispatched(e, origin);
}

} // namespace gvt

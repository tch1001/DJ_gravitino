#include "Flx4TutorialMap.h"

#include <array>
#include <cmath>

namespace gvt {

std::optional<Flx4TutorialMapping> flx4TutorialMapping(
    ControlId id, double value) noexcept
{
    const auto simple = [](Flx4SurfaceControl surface) {
        return std::optional<Flx4TutorialMapping>(
            Flx4TutorialMapping {surface, -1, Flx4PadMode::None, false});
    };
    const auto fx = [](Flx4SurfaceControl surface) {
        return std::optional<Flx4TutorialMapping>(
            Flx4TutorialMapping {surface, -1, Flx4PadMode::None, true});
    };

    switch (id) {
    case ControlId::Play:
    case ControlId::Stop:         return simple(Flx4SurfaceControl::PlayPause);
    case ControlId::Cue:          return simple(Flx4SurfaceControl::Cue);
    case ControlId::Load:         return simple(Flx4SurfaceControl::Load);
    case ControlId::TempoSync:    return simple(Flx4SurfaceControl::Sync);
    case ControlId::LoopIn:       return simple(Flx4SurfaceControl::LoopIn);
    case ControlId::LoopOut:      return simple(Flx4SurfaceControl::LoopOut);
    case ControlId::LoopExit:     return simple(Flx4SurfaceControl::FourBeatExit);
    case ControlId::LoopHalve:    return simple(Flx4SurfaceControl::LoopHalve);
    case ControlId::LoopDouble:   return simple(Flx4SurfaceControl::LoopDouble);
    case ControlId::Tempo:        return simple(Flx4SurfaceControl::TempoFader);
    case ControlId::Fader:        return simple(Flx4SurfaceControl::ChannelFader);
    case ControlId::Trim:         return simple(Flx4SurfaceControl::Trim);
    case ControlId::EqHigh:       return simple(Flx4SurfaceControl::EqHigh);
    case ControlId::EqMid:        return simple(Flx4SurfaceControl::EqMid);
    case ControlId::EqLow:        return simple(Flx4SurfaceControl::EqLow);
    case ControlId::Filter:       return simple(Flx4SurfaceControl::Filter);
    case ControlId::Crossfader:   return simple(Flx4SurfaceControl::Crossfader);
    case ControlId::HeadphoneCue: return simple(Flx4SurfaceControl::ChannelCue);
    case ControlId::MasterCue:    return simple(Flx4SurfaceControl::MasterCue);
    case ControlId::HeadphoneMix: return simple(Flx4SurfaceControl::HeadphoneMix);
    case ControlId::Quantize:     return simple(Flx4SurfaceControl::ChannelCue);
    case ControlId::Jog:
    case ControlId::PlatterScratch:
    case ControlId::PlatterTouch:
        return simple(Flx4SurfaceControl::JogWheel);
    case ControlId::FxType:       return fx(Flx4SurfaceControl::BeatFxSelect);
    case ControlId::FxOn:         return fx(Flx4SurfaceControl::BeatFxOn);
    case ControlId::FxWet:        return fx(Flx4SurfaceControl::BeatFxWet);
    case ControlId::FxBeats:      return fx(Flx4SurfaceControl::BeatFxBeats);

    case ControlId::HotCue1:
    case ControlId::HotCue2:
    case ControlId::HotCue3:
    case ControlId::HotCue4:
    case ControlId::HotCue5:
    case ControlId::HotCue6:
    case ControlId::HotCue7:
    case ControlId::HotCue8: {
        const int pad = static_cast<int>(id) -
                        static_cast<int>(ControlId::HotCue1);
        return Flx4TutorialMapping {Flx4SurfaceControl::PerformancePad,
                                    pad, Flx4PadMode::HotCue, false};
    }

    case ControlId::SavedLoop1:
    case ControlId::SavedLoop2:
    case ControlId::SavedLoop3:
    case ControlId::SavedLoop4:
    case ControlId::SavedLoop5:
    case ControlId::SavedLoop6:
    case ControlId::SavedLoop7:
    case ControlId::SavedLoop8: {
        const int pad = static_cast<int>(id) -
                        static_cast<int>(ControlId::SavedLoop1);
        return Flx4TutorialMapping {Flx4SurfaceControl::PerformancePad,
                                    pad, Flx4PadMode::Custom, false};
    }

    case ControlId::LoopAuto:
        // The mapped FLX4 button is specifically 4 BEAT/EXIT. Other auto-loop
        // lengths exist in Gravitino's screen UI but have no single mapped
        // FLX4 gesture.
        if (std::fabs(value - 4.0) < 0.01)
            return simple(Flx4SurfaceControl::FourBeatExit);
        return std::nullopt;

    case ControlId::BeatJump: {
        constexpr std::array<double, 8> jumps {
            -1.0, 1.0, -2.0, 2.0, -4.0, 4.0, -8.0, 8.0};
        for (int pad = 0; pad < static_cast<int>(jumps.size()); ++pad) {
            if (std::fabs(value - jumps[static_cast<std::size_t>(pad)]) < 0.01)
                return Flx4TutorialMapping {
                    Flx4SurfaceControl::PerformancePad, pad,
                    Flx4PadMode::BeatJump, false};
        }
        return std::nullopt;
    }

    case ControlId::StemVocals:
    case ControlId::StemMelody:
    case ControlId::StemBass:
    case ControlId::StemDrums:
    case ControlId::BrowseSelect:
    case ControlId::BrowseNavigate:
    case ControlId::PerformancePadMode:
    case ControlId::PerformancePad1:
    case ControlId::PerformancePad2:
    case ControlId::PerformancePad3:
    case ControlId::PerformancePad4:
    case ControlId::PerformancePad5:
    case ControlId::PerformancePad6:
    case ControlId::PerformancePad7:
    case ControlId::PerformancePad8:
    case ControlId::Count:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace gvt

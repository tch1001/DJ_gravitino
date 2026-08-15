#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace gvt {

// Allocation-free per-deck insert effects. All delay storage is part of this
// object and is constructed with Deck::Impl, never in the audio callback.
class DeckFx final {
public:
    DeckFx() noexcept;

    void reset() noexcept;
    bool hasTail() const noexcept
    {
        return hasTail_.load(std::memory_order_relaxed);
    }

    // Processes interleaved stereo in place. When engaged is false but a tail
    // remains, dry audio passes normally while the effect input is muted and
    // the stored tail is allowed to decay.
    void process(float* interleavedStereo, int frames, int type,
                 bool engaged, float wet, double beats,
                 double effectiveBpm) noexcept;

private:
    static constexpr std::size_t kEchoFrames = 2U * 48000U;
    static constexpr std::size_t kReverbCombMaxFrames = 2200U;
    static constexpr std::size_t kReverbAllpassMaxFrames = 256U;
    static constexpr std::size_t kFlangerFrames = 386U;

    struct CombDelay {
        std::array<float, kReverbCombMaxFrames> samples {};
        std::size_t length = 1;
        std::size_t write = 0;
    };

    struct AllpassDelay {
        std::array<float, kReverbAllpassMaxFrames> samples {};
        std::size_t length = 1;
        std::size_t write = 0;
    };

    void clearEffectState() noexcept;
    void processEcho(float* samples, int frames, bool feedInput,
                     double delayFrames, float targetDry,
                     float targetWet) noexcept;
    void processReverb(float* samples, int frames, bool feedInput,
                       float targetDry, float targetWet) noexcept;
    void processFlanger(float* samples, int frames, bool feedInput,
                        double lfoPeriodFrames, float targetDry,
                        float targetWet) noexcept;
    void mixFrame(float& left, float& right, float wetLeft, float wetRight,
                  float targetDry, float targetWet) noexcept;

    std::array<float, kEchoFrames * 2U> echoRing_ {};
    std::size_t echoWrite_ = 0;
    double echoDelayFrames_ = 0.0;
    double echoEnergy_ = 0.0;

    std::array<std::array<CombDelay, 4>, 2> reverbCombs_ {};
    std::array<std::array<AllpassDelay, 2>, 2> reverbAllpasses_ {};
    double reverbEnergy_ = 0.0;

    std::array<float, kFlangerFrames * 2U> flangerRing_ {};
    std::size_t flangerWrite_ = 0;
    double flangerPhase_ = 0.0;
    double flangerEnergy_ = 0.0;

    int activeType_ = -1;
    std::atomic<bool> hasTail_ {false};
    float smoothedDryGain_ = 1.0f;
    float smoothedWetGain_ = 0.0f;
};

} // namespace gvt

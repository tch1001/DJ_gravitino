#include "Fx.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gvt {
namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kEchoFeedback = 0.5;
constexpr double kReverbFeedback = 0.77;
constexpr double kAllpassGain = 0.7;
constexpr double kFlangerFeedback = 0.6;
constexpr double kMinimumDelaySec = 0.001;
constexpr double kMaximumDelaySec = 2.0;
constexpr double kMinimumFlangerSec = 0.0005;
constexpr double kMaximumFlangerSec = 0.008;
constexpr double kStateEnergyThreshold = 1.0e-10;
constexpr float kMixSmoothing = 0.004158004f; // approximately 5 ms at 48 kHz
constexpr double kDelaySmoothing = 0.001040583; // approximately 20 ms

float finiteUnit(float value, float fallback) noexcept
{
    if (!std::isfinite(value))
        return fallback;
    return std::clamp(value, 0.0f, 1.0f);
}

double finiteBpm(double bpm) noexcept
{
    return std::isfinite(bpm) && bpm > 0.0 ? bpm : 120.0;
}

double finiteBeats(double beats) noexcept
{
    if (!std::isfinite(beats))
        return 0.5;
    return std::clamp(beats, 0.25, 4.0);
}

float zapDenormal(float value) noexcept
{
    return std::abs(value) < 1.0e-12f ? 0.0f : value;
}

void replaceDelaySample(float& slot, float value, double& energy) noexcept
{
    value = zapDenormal(value);
    const double oldValue = static_cast<double>(slot);
    const double newValue = static_cast<double>(value);
    energy += newValue * newValue - oldValue * oldValue;
    if (energy < 0.0 && energy > -1.0e-12)
        energy = 0.0;
    slot = value;
}

} // namespace

DeckFx::DeckFx() noexcept
{
    constexpr std::array<std::size_t, 4> leftCombLengths {
        1426U, 1781U, 1973U, 2098U};
    constexpr std::array<std::size_t, 4> rightCombLengths {
        1443U, 1807U, 2003U, 2131U};
    constexpr std::array<std::size_t, 2> leftAllpassLengths {240U, 82U};
    constexpr std::array<std::size_t, 2> rightAllpassLengths {247U, 85U};

    for (std::size_t index = 0; index < leftCombLengths.size(); ++index) {
        reverbCombs_[0][index].length = leftCombLengths[index];
        reverbCombs_[1][index].length = rightCombLengths[index];
    }
    for (std::size_t index = 0; index < leftAllpassLengths.size(); ++index) {
        reverbAllpasses_[0][index].length = leftAllpassLengths[index];
        reverbAllpasses_[1][index].length = rightAllpassLengths[index];
    }
}

void DeckFx::reset() noexcept
{
    clearEffectState();
    activeType_ = -1;
    smoothedDryGain_ = 1.0f;
    smoothedWetGain_ = 0.0f;
}

void DeckFx::clearEffectState() noexcept
{
    echoRing_.fill(0.0f);
    echoWrite_ = 0;
    echoDelayFrames_ = 0.0;
    echoEnergy_ = 0.0;

    for (auto& channel : reverbCombs_) {
        for (auto& comb : channel) {
            comb.samples.fill(0.0f);
            comb.write = 0;
        }
    }
    for (auto& channel : reverbAllpasses_) {
        for (auto& allpass : channel) {
            allpass.samples.fill(0.0f);
            allpass.write = 0;
        }
    }
    reverbEnergy_ = 0.0;

    flangerRing_.fill(0.0f);
    flangerWrite_ = 0;
    flangerPhase_ = 0.0;
    flangerEnergy_ = 0.0;
    hasTail_.store(false, std::memory_order_relaxed);
}

void DeckFx::process(float* samples, int frames, int type, bool engaged,
                     float wet, double beats, double effectiveBpm) noexcept
{
    if (samples == nullptr || frames <= 0)
        return;

    type = std::clamp(type, 0, 2);
    if (!engaged && !hasTail()) {
        // Remember selection changes without touching any delay storage. This
        // is the steady-state hard bypass path used by an idle FX insert.
        activeType_ = type;
        smoothedDryGain_ = 1.0f;
        smoothedWetGain_ = 0.0f;
        return;
    }

    if (type != activeType_) {
        clearEffectState();
        activeType_ = type;
    }

    wet = finiteUnit(wet, 0.5f);
    const float angle = wet * std::numbers::pi_v<float> * 0.5f;
    // Once disengaged, restore unity dry while retaining the selected wet-tail
    // level. This is the conventional echo-out behavior and avoids ducking the
    // continuing dry deck until a long delay/reverb tail reaches silence.
    const float targetDry = engaged ? std::cos(angle) : 1.0f;
    const float targetWet = std::sin(angle);
    const double bpm = finiteBpm(effectiveBpm);
    const double beatCount = finiteBeats(beats);

    switch (type) {
    case 0: {
        const double delayFrames = std::clamp(
            beatCount * 60.0 / bpm * kSampleRate,
            kMinimumDelaySec * kSampleRate,
            kMaximumDelaySec * kSampleRate);
        processEcho(samples, frames, engaged, delayFrames,
                    targetDry, targetWet);
        hasTail_.store(echoEnergy_ > kStateEnergyThreshold,
                       std::memory_order_relaxed);
        break;
    }
    case 1:
        processReverb(samples, frames, engaged, targetDry, targetWet);
        hasTail_.store(reverbEnergy_ > kStateEnergyThreshold,
                       std::memory_order_relaxed);
        break;
    case 2: {
        const double periodFrames = std::max(
            1.0, beatCount * 4.0 * 60.0 / bpm * kSampleRate);
        processFlanger(samples, frames, engaged, periodFrames,
                       targetDry, targetWet);
        hasTail_.store(flangerEnergy_ > kStateEnergyThreshold,
                       std::memory_order_relaxed);
        break;
    }
    default:
        break;
    }
}

void DeckFx::mixFrame(float& left, float& right, float wetLeft,
                      float wetRight, float targetDry,
                      float targetWet) noexcept
{
    smoothedDryGain_ +=
        (targetDry - smoothedDryGain_) * kMixSmoothing;
    smoothedWetGain_ +=
        (targetWet - smoothedWetGain_) * kMixSmoothing;
    left = left * smoothedDryGain_ + wetLeft * smoothedWetGain_;
    right = right * smoothedDryGain_ + wetRight * smoothedWetGain_;
}

void DeckFx::processEcho(float* samples, int frames, bool feedInput,
                         double targetDelayFrames, float targetDry,
                         float targetWet) noexcept
{
    if (echoDelayFrames_ <= 0.0)
        echoDelayFrames_ = targetDelayFrames;

    for (int frame = 0; frame < frames; ++frame) {
        echoDelayFrames_ +=
            (targetDelayFrames - echoDelayFrames_) * kDelaySmoothing;

        double readPosition = static_cast<double>(echoWrite_) - echoDelayFrames_;
        while (readPosition < 0.0)
            readPosition += static_cast<double>(kEchoFrames);
        while (readPosition >= static_cast<double>(kEchoFrames))
            readPosition -= static_cast<double>(kEchoFrames);
        const auto read0 = static_cast<std::size_t>(readPosition);
        const auto read1 = (read0 + 1U) % kEchoFrames;
        const float fraction = static_cast<float>(
            readPosition - static_cast<double>(read0));
        const std::size_t outputBase = static_cast<std::size_t>(frame) * 2U;

        float dry[2] {samples[outputBase], samples[outputBase + 1U]};
        float delayed[2] {};
        for (std::size_t channel = 0; channel < 2U; ++channel) {
            const float first = echoRing_[read0 * 2U + channel];
            const float second = echoRing_[read1 * 2U + channel];
            delayed[channel] = first + (second - first) * fraction;
            const float input = feedInput ? dry[channel] : 0.0f;
            replaceDelaySample(
                echoRing_[echoWrite_ * 2U + channel],
                input + static_cast<float>(kEchoFeedback) * delayed[channel],
                echoEnergy_);
        }

        mixFrame(dry[0], dry[1], delayed[0], delayed[1],
                 targetDry, targetWet);
        samples[outputBase] = dry[0];
        samples[outputBase + 1U] = dry[1];
        echoWrite_ = (echoWrite_ + 1U) % kEchoFrames;
    }
}

void DeckFx::processReverb(float* samples, int frames, bool feedInput,
                           float targetDry, float targetWet) noexcept
{
    for (int frame = 0; frame < frames; ++frame) {
        const std::size_t base = static_cast<std::size_t>(frame) * 2U;
        float dry[2] {samples[base], samples[base + 1U]};
        float wet[2] {};
        for (std::size_t channel = 0; channel < 2U; ++channel) {
            const float input = feedInput ? dry[channel] : 0.0f;
            float parallel = 0.0f;
            for (auto& comb : reverbCombs_[channel]) {
                const float delayed = comb.samples[comb.write];
                replaceDelaySample(
                    comb.samples[comb.write],
                    input + static_cast<float>(kReverbFeedback) * delayed,
                    reverbEnergy_);
                comb.write = (comb.write + 1U) % comb.length;
                parallel += delayed;
            }

            float value = parallel * 0.25f;
            for (auto& allpass : reverbAllpasses_[channel]) {
                const float delayed = allpass.samples[allpass.write];
                const float output = delayed -
                    static_cast<float>(kAllpassGain) * value;
                replaceDelaySample(
                    allpass.samples[allpass.write],
                    value + static_cast<float>(kAllpassGain) * output,
                    reverbEnergy_);
                allpass.write = (allpass.write + 1U) % allpass.length;
                value = output;
            }
            wet[channel] = value;
        }
        mixFrame(dry[0], dry[1], wet[0], wet[1], targetDry, targetWet);
        samples[base] = dry[0];
        samples[base + 1U] = dry[1];
    }
}

void DeckFx::processFlanger(float* samples, int frames, bool feedInput,
                            double lfoPeriodFrames, float targetDry,
                            float targetWet) noexcept
{
    const double phaseStep = 2.0 * std::numbers::pi / lfoPeriodFrames;
    constexpr double kDelayCenterSec =
        (kMinimumFlangerSec + kMaximumFlangerSec) * 0.5;
    constexpr double kDelayDepthSec =
        (kMaximumFlangerSec - kMinimumFlangerSec) * 0.5;

    for (int frame = 0; frame < frames; ++frame) {
        const std::size_t base = static_cast<std::size_t>(frame) * 2U;
        float dry[2] {samples[base], samples[base + 1U]};
        float delayed[2] {};

        for (std::size_t channel = 0; channel < 2U; ++channel) {
            const double phase = flangerPhase_ +
                (channel == 0U ? 0.0 : std::numbers::pi);
            const double delayFrames =
                (kDelayCenterSec + kDelayDepthSec * std::sin(phase)) *
                kSampleRate;
            double readPosition =
                static_cast<double>(flangerWrite_) - delayFrames;
            while (readPosition < 0.0)
                readPosition += static_cast<double>(kFlangerFrames);
            const auto read0 = static_cast<std::size_t>(readPosition);
            const auto read1 = (read0 + 1U) % kFlangerFrames;
            const float fraction = static_cast<float>(
                readPosition - static_cast<double>(read0));
            const float first = flangerRing_[read0 * 2U + channel];
            const float second = flangerRing_[read1 * 2U + channel];
            delayed[channel] = first + (second - first) * fraction;
            const float input = feedInput ? dry[channel] : 0.0f;
            replaceDelaySample(
                flangerRing_[flangerWrite_ * 2U + channel],
                input + static_cast<float>(kFlangerFeedback) * delayed[channel],
                flangerEnergy_);
        }

        mixFrame(dry[0], dry[1], delayed[0], delayed[1],
                 targetDry, targetWet);
        samples[base] = dry[0];
        samples[base + 1U] = dry[1];
        flangerWrite_ = (flangerWrite_ + 1U) % kFlangerFrames;
        flangerPhase_ += phaseStep;
        if (flangerPhase_ >= 2.0 * std::numbers::pi)
            flangerPhase_ = std::fmod(flangerPhase_, 2.0 * std::numbers::pi);
    }
}

} // namespace gvt

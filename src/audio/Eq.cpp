#include "Eq.h"

#include "../analysis/TrackData.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gvt {
namespace {

constexpr double kLowFrequency = 250.0;
constexpr double kMidFrequency = 1000.0;
constexpr double kHighFrequency = 4000.0;
constexpr double kShelfSlope = 1.0;
constexpr double kMidQ = 0.8;
constexpr double kKillDb = -26.0;
constexpr double kBoostDb = 9.0;
constexpr double kFilterQ = 0.7071067811865476;
constexpr double kFilterLowPassMaximumHz = 20000.0;
constexpr double kFilterLowPassMinimumHz = 120.0;
constexpr double kFilterHighPassMinimumHz = 20.0;
constexpr double kFilterHighPassMaximumHz = 8000.0;
constexpr float kFilterLowEdge = 0.47f;
constexpr float kFilterHighEdge = 0.53f;

} // namespace

Eq::Eq() noexcept = default;

float StereoBiquad::process(float input, int channel) noexcept
{
    const double x = static_cast<double>(input);
    const auto index = static_cast<std::size_t>(channel);
    const double output = b0_ * x + z1_[index];
    z1_[index] = b1_ * x - a1_ * output + z2_[index];
    z2_[index] = b2_ * x - a2_ * output;

    // Keep offline rendering from accumulating denormals after filter tails.
    if (std::abs(z1_[index]) < 1.0e-30)
        z1_[index] = 0.0;
    if (std::abs(z2_[index]) < 1.0e-30)
        z2_[index] = 0.0;

    return static_cast<float>(output);
}

void StereoBiquad::reset() noexcept
{
    z1_.fill(0.0);
    z2_.fill(0.0);
}

void StereoBiquad::setIdentity() noexcept
{
    b0_ = 1.0;
    b1_ = 0.0;
    b2_ = 0.0;
    a1_ = 0.0;
    a2_ = 0.0;
}

void StereoBiquad::setCoefficients(
    double b0, double b1, double b2,
    double a0, double a1, double a2) noexcept
{
    const double inverseA0 = 1.0 / a0;
    b0_ = b0 * inverseA0;
    b1_ = b1 * inverseA0;
    b2_ = b2 * inverseA0;
    a1_ = a1 * inverseA0;
    a2_ = a2 * inverseA0;
}

void StereoBiquad::setLowPass(double frequencyHz, double q) noexcept
{
    const double nyquist = static_cast<double>(kSampleRate) * 0.5;
    frequencyHz = std::clamp(frequencyHz, 1.0, nyquist * 0.999);
    q = std::max(q, 1.0e-6);

    const double omega = 2.0 * std::numbers::pi * frequencyHz /
                         static_cast<double>(kSampleRate);
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * q);
    setCoefficients((1.0 - cosine) * 0.5, 1.0 - cosine,
                    (1.0 - cosine) * 0.5, 1.0 + alpha,
                    -2.0 * cosine, 1.0 - alpha);
}

void StereoBiquad::setHighPass(double frequencyHz, double q) noexcept
{
    const double nyquist = static_cast<double>(kSampleRate) * 0.5;
    frequencyHz = std::clamp(frequencyHz, 1.0, nyquist * 0.999);
    q = std::max(q, 1.0e-6);

    const double omega = 2.0 * std::numbers::pi * frequencyHz /
                         static_cast<double>(kSampleRate);
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * q);
    setCoefficients((1.0 + cosine) * 0.5, -(1.0 + cosine),
                    (1.0 + cosine) * 0.5, 1.0 + alpha,
                    -2.0 * cosine, 1.0 - alpha);
}

void Eq::reset() noexcept
{
    low_.reset();
    mid_.reset();
    high_.reset();
}

float Eq::sanitizeKnob(float knob) noexcept
{
    if (!std::isfinite(knob))
        return 0.5f;
    return std::clamp(knob, 0.0f, 1.0f);
}

double Eq::gainDb(float knob) noexcept
{
    if (knob <= 0.5f)
        return kKillDb * (1.0 - static_cast<double>(knob) * 2.0);
    return kBoostDb * (static_cast<double>(knob) - 0.5) * 2.0;
}

void Eq::setLowShelf(StereoBiquad& filter, double db) noexcept
{
    if (std::abs(db) < 1.0e-12) {
        filter.setIdentity();
        return;
    }

    const double a = std::pow(10.0, db / 40.0);
    const double omega = 2.0 * std::numbers::pi * kLowFrequency /
                         static_cast<double>(kSampleRate);
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    const double alpha = sine * 0.5 *
        std::sqrt((a + 1.0 / a) * (1.0 / kShelfSlope - 1.0) + 2.0);
    const double shelfTerm = 2.0 * std::sqrt(a) * alpha;

    filter.setCoefficients(
        a * ((a + 1.0) - (a - 1.0) * cosine + shelfTerm),
        2.0 * a * ((a - 1.0) - (a + 1.0) * cosine),
        a * ((a + 1.0) - (a - 1.0) * cosine - shelfTerm),
        (a + 1.0) + (a - 1.0) * cosine + shelfTerm,
        -2.0 * ((a - 1.0) + (a + 1.0) * cosine),
        (a + 1.0) + (a - 1.0) * cosine - shelfTerm);
}

void Eq::setPeak(StereoBiquad& filter, double db) noexcept
{
    if (std::abs(db) < 1.0e-12) {
        filter.setIdentity();
        return;
    }

    const double a = std::pow(10.0, db / 40.0);
    const double omega = 2.0 * std::numbers::pi * kMidFrequency /
                         static_cast<double>(kSampleRate);
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * kMidQ);

    filter.setCoefficients(1.0 + alpha * a, -2.0 * cosine,
                           1.0 - alpha * a, 1.0 + alpha / a,
                           -2.0 * cosine, 1.0 - alpha / a);
}

void Eq::setHighShelf(StereoBiquad& filter, double db) noexcept
{
    if (std::abs(db) < 1.0e-12) {
        filter.setIdentity();
        return;
    }

    const double a = std::pow(10.0, db / 40.0);
    const double omega = 2.0 * std::numbers::pi * kHighFrequency /
                         static_cast<double>(kSampleRate);
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    const double alpha = sine * 0.5 *
        std::sqrt((a + 1.0 / a) * (1.0 / kShelfSlope - 1.0) + 2.0);
    const double shelfTerm = 2.0 * std::sqrt(a) * alpha;

    filter.setCoefficients(
        a * ((a + 1.0) + (a - 1.0) * cosine + shelfTerm),
        -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine),
        a * ((a + 1.0) + (a - 1.0) * cosine - shelfTerm),
        (a + 1.0) - (a - 1.0) * cosine + shelfTerm,
        2.0 * ((a - 1.0) - (a + 1.0) * cosine),
        (a + 1.0) - (a - 1.0) * cosine - shelfTerm);
}

void Eq::process(float* samples, int frames, float low, float mid,
                 float high) noexcept
{
    if (samples == nullptr || frames <= 0)
        return;

    low = sanitizeKnob(low);
    mid = sanitizeKnob(mid);
    high = sanitizeKnob(high);

    if (low != lastLow_) {
        setLowShelf(low_, gainDb(low));
        lastLow_ = low;
    }
    if (mid != lastMid_) {
        setPeak(mid_, gainDb(mid));
        lastMid_ = mid;
    }
    if (high != lastHigh_) {
        setHighShelf(high_, gainDb(high));
        lastHigh_ = high;
    }

    for (int frame = 0; frame < frames; ++frame) {
        const int base = frame * 2;
        for (int channel = 0; channel < 2; ++channel) {
            float sample = samples[base + channel];
            sample = low_.process(sample, channel);
            sample = mid_.process(sample, channel);
            samples[base + channel] = high_.process(sample, channel);
        }
    }
}

void DjFilter::reset() noexcept
{
    biquad_.reset();
    biquad_.setIdentity();
    lastKnob_ = -1.0f;
    lastMode_ = 0;
}

void DjFilter::process(float* samples, int frames, float knob) noexcept
{
    if (samples == nullptr || frames <= 0)
        return;

    if (!std::isfinite(knob))
        knob = 0.5f;
    knob = std::clamp(knob, 0.0f, 1.0f);

    const int mode = knob < kFilterLowEdge ? -1
        : (knob > kFilterHighEdge ? 1 : 0);
    if (mode == 0) {
        if (lastMode_ != 0)
            biquad_.reset();
        lastMode_ = 0;
        lastKnob_ = knob;
        return;
    }

    if (mode != lastMode_)
        biquad_.reset();

    if (knob != lastKnob_ || mode != lastMode_) {
        if (mode < 0) {
            const double sweep =
                (static_cast<double>(kFilterLowEdge) - knob) /
                static_cast<double>(kFilterLowEdge);
            const double cutoff = kFilterLowPassMaximumHz * std::pow(
                kFilterLowPassMinimumHz / kFilterLowPassMaximumHz, sweep);
            biquad_.setLowPass(cutoff, kFilterQ);
        } else {
            const double sweep =
                (knob - static_cast<double>(kFilterHighEdge)) /
                (1.0 - static_cast<double>(kFilterHighEdge));
            const double cutoff = kFilterHighPassMinimumHz * std::pow(
                kFilterHighPassMaximumHz / kFilterHighPassMinimumHz, sweep);
            biquad_.setHighPass(cutoff, kFilterQ);
        }
        lastKnob_ = knob;
        lastMode_ = mode;
    }

    for (int frame = 0; frame < frames; ++frame) {
        const int base = frame * 2;
        for (int channel = 0; channel < 2; ++channel)
            samples[base + channel] = biquad_.process(
                samples[base + channel], channel);
    }
}

} // namespace gvt

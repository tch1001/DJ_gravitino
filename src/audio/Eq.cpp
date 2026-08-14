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

} // namespace

Eq::Eq() noexcept = default;

float Eq::Biquad::process(float input, int channel) noexcept
{
    const double x = static_cast<double>(input);
    const double output = b0 * x + z1[static_cast<std::size_t>(channel)];
    z1[static_cast<std::size_t>(channel)] =
        b1 * x - a1 * output + z2[static_cast<std::size_t>(channel)];
    z2[static_cast<std::size_t>(channel)] = b2 * x - a2 * output;

    // Keep offline rendering from accumulating denormals after filter tails.
    if (std::abs(z1[static_cast<std::size_t>(channel)]) < 1.0e-30)
        z1[static_cast<std::size_t>(channel)] = 0.0;
    if (std::abs(z2[static_cast<std::size_t>(channel)]) < 1.0e-30)
        z2[static_cast<std::size_t>(channel)] = 0.0;

    return static_cast<float>(output);
}

void Eq::Biquad::reset() noexcept
{
    z1.fill(0.0);
    z2.fill(0.0);
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

void Eq::normalize(Biquad& filter, double b0, double b1, double b2,
                   double a0, double a1, double a2) noexcept
{
    const double inverseA0 = 1.0 / a0;
    filter.b0 = b0 * inverseA0;
    filter.b1 = b1 * inverseA0;
    filter.b2 = b2 * inverseA0;
    filter.a1 = a1 * inverseA0;
    filter.a2 = a2 * inverseA0;
}

void Eq::setIdentity(Biquad& filter) noexcept
{
    filter.b0 = 1.0;
    filter.b1 = 0.0;
    filter.b2 = 0.0;
    filter.a1 = 0.0;
    filter.a2 = 0.0;
}

void Eq::setLowShelf(Biquad& filter, double db) noexcept
{
    if (std::abs(db) < 1.0e-12) {
        setIdentity(filter);
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

    normalize(filter,
              a * ((a + 1.0) - (a - 1.0) * cosine + shelfTerm),
              2.0 * a * ((a - 1.0) - (a + 1.0) * cosine),
              a * ((a + 1.0) - (a - 1.0) * cosine - shelfTerm),
              (a + 1.0) + (a - 1.0) * cosine + shelfTerm,
              -2.0 * ((a - 1.0) + (a + 1.0) * cosine),
              (a + 1.0) + (a - 1.0) * cosine - shelfTerm);
}

void Eq::setPeak(Biquad& filter, double db) noexcept
{
    if (std::abs(db) < 1.0e-12) {
        setIdentity(filter);
        return;
    }

    const double a = std::pow(10.0, db / 40.0);
    const double omega = 2.0 * std::numbers::pi * kMidFrequency /
                         static_cast<double>(kSampleRate);
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * kMidQ);

    normalize(filter,
              1.0 + alpha * a,
              -2.0 * cosine,
              1.0 - alpha * a,
              1.0 + alpha / a,
              -2.0 * cosine,
              1.0 - alpha / a);
}

void Eq::setHighShelf(Biquad& filter, double db) noexcept
{
    if (std::abs(db) < 1.0e-12) {
        setIdentity(filter);
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

    normalize(filter,
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

} // namespace gvt

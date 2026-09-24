// Turn sparse platter position reports into continuous vinyl-like motion and
// band-limit the source before reading it at a variable (possibly reverse) rate.
// All state is audio-thread-owned; lookup tables are prepared at deck creation.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace gvt {

class ScratchMotion {
public:
    void reset(double position) noexcept { intermediate_ = position; }

    double advance(double position, double target) noexcept
    {
        // Two cascaded 6 ms position smoothers bridge MIDI packet gaps without
        // predicting motion beyond the hand's position. About 12 ms group delay,
        // sample-clock based, independent of the audio callback's block size.
        constexpr double alpha = 0.003466201029630846; // 1-exp(-1/(48000*.006))
        intermediate_ += alpha * (target - intermediate_);
        const double step = std::clamp(alpha * (intermediate_ - position),
                                      -12.0, 12.0);
        return step;
    }

private:
    double intermediate_ = 0.0;
};

class ScratchResampler {
public:
    ScratchResampler() : tables_(tables()) {}

    template<class ReadFrame>
    std::array<float, 2> sample(double position, double speed,
                                ReadFrame&& readFrame) const noexcept
    {
        // A speed-dependent low-pass in SOURCE space is essential: filtering
        // after resampling cannot remove frequencies which have already aliased.
        const double cutoff = 0.9 / std::max(1.0, std::abs(speed));
        const double base = std::floor(position);
        const double fraction = position - base;
        std::array<double, 2> sum {};
        double weightSum = 0.0;
        for (int tap = 1 - radius; tap <= radius; ++tap) {
            const double distance = std::abs(tap - fraction);
            const double weight = lookup(tables_.sinc, distance * cutoff) *
                                  lookup(tables_.window, distance);
            const auto frame = readFrame(base + tap);
            sum[0] += frame[0] * weight;
            sum[1] += frame[1] * weight;
            weightSum += weight;
        }
        return {static_cast<float>(sum[0] / weightSum),
                static_cast<float>(sum[1] / weightSum)};
    }

private:
    static constexpr int radius = 32;
    static constexpr int tableSize = 8192;
    struct Tables {
        std::array<double, tableSize + 1> sinc {}, window {};
        Tables()
        {
            for (int i = 0; i <= tableSize; ++i) {
                const double x = static_cast<double>(i) * radius / tableSize;
                const double angle = std::numbers::pi * x;
                sinc[i] = i == 0 ? 1.0 : std::sin(angle) / angle;
                window[i] = 0.42 + 0.5 * std::cos(angle / radius) +
                            0.08 * std::cos(2.0 * angle / radius);
            }
        }
    };
    static const Tables& tables()
    {
        static const Tables value;
        return value;
    }
    static double lookup(const std::array<double, tableSize + 1>& table,
                         double distance) noexcept
    {
        const double index = std::clamp(distance * (tableSize / radius),
                                        0.0, static_cast<double>(tableSize));
        const int lower = std::min(static_cast<int>(index), tableSize - 1);
        return table[lower] + (table[lower + 1] - table[lower]) * (index - lower);
    }
    const Tables& tables_;
};

} // namespace gvt

#pragma once

#include <array>

namespace gvt {

// Three-band stereo DJ EQ. Knobs are normalized: 0.5 is flat, 0 is a
// -26 dB kill, and 1 is +9 dB.
class Eq {
public:
    Eq() noexcept;

    void reset() noexcept;
    void process(float* interleavedStereo, int frames,
                 float low, float mid, float high) noexcept;

private:
    struct Biquad {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        std::array<double, 2> z1 {};
        std::array<double, 2> z2 {};

        float process(float input, int channel) noexcept;
        void reset() noexcept;
    };

    Biquad low_;
    Biquad mid_;
    Biquad high_;
    float lastLow_ = -1.0f;
    float lastMid_ = -1.0f;
    float lastHigh_ = -1.0f;

    static float sanitizeKnob(float knob) noexcept;
    static double gainDb(float knob) noexcept;
    static void normalize(Biquad& filter, double b0, double b1, double b2,
                          double a0, double a1, double a2) noexcept;
    static void setIdentity(Biquad& filter) noexcept;
    static void setLowShelf(Biquad& filter, double gainDb) noexcept;
    static void setPeak(Biquad& filter, double gainDb) noexcept;
    static void setHighShelf(Biquad& filter, double gainDb) noexcept;
};

} // namespace gvt

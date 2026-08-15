#pragma once

#include <array>

namespace gvt {

// Small stereo transposed-direct-form-II biquad shared by the EQ and the
// post-EQ DJ filter. Coefficients may be updated on the audio thread without
// allocation; delay state is maintained independently per channel.
struct StereoBiquad {
    float process(float input, int channel) noexcept;
    void reset() noexcept;
    void setIdentity() noexcept;
    void setCoefficients(double b0, double b1, double b2,
                         double a0, double a1, double a2) noexcept;
    void setLowPass(double frequencyHz, double q) noexcept;
    void setHighPass(double frequencyHz, double q) noexcept;

private:
    double b0_ = 1.0;
    double b1_ = 0.0;
    double b2_ = 0.0;
    double a1_ = 0.0;
    double a2_ = 0.0;
    std::array<double, 2> z1_ {};
    std::array<double, 2> z2_ {};
};

// Three-band stereo DJ EQ. Knobs are normalized: 0.5 is flat, 0 is a
// -26 dB kill, and 1 is +9 dB.
class Eq {
public:
    Eq() noexcept;

    void reset() noexcept;
    void process(float* interleavedStereo, int frames,
                 float low, float mid, float high) noexcept;

private:
    StereoBiquad low_;
    StereoBiquad mid_;
    StereoBiquad high_;
    float lastLow_ = -1.0f;
    float lastMid_ = -1.0f;
    float lastHigh_ = -1.0f;

    static float sanitizeKnob(float knob) noexcept;
    static double gainDb(float knob) noexcept;
    static void setLowShelf(StereoBiquad& filter, double gainDb) noexcept;
    static void setPeak(StereoBiquad& filter, double gainDb) noexcept;
    static void setHighShelf(StereoBiquad& filter, double gainDb) noexcept;
};

// Pioneer-style bipolar channel filter. The center dead-zone is a true bypass;
// counter-clockwise selects a low-pass and clockwise selects a high-pass.
class DjFilter {
public:
    void reset() noexcept;
    void process(float* interleavedStereo, int frames, float knob) noexcept;

private:
    StereoBiquad biquad_;
    float lastKnob_ = -1.0f;
    int lastMode_ = 0; // -1 = low-pass, 0 = bypass, +1 = high-pass
};

} // namespace gvt

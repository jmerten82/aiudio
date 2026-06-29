// aiudio-graph — BiquadNode: 1 in → 1 out biquad filter (G4). A real classic-DSP
// node with per-channel state, using a Transposed Direct Form II difference
// equation, plus RBJ-cookbook low-/high-pass coefficient helpers. State is
// allocated at construction (up to maxChannels); process() is allocation-free.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class BiquadNode final : public Node {
public:
    explicit BiquadNode(std::uint32_t maxChannels = 2)
        : z1_(maxChannels == 0 ? 1 : maxChannels, 0.0f),
          z2_(maxChannels == 0 ? 1 : maxChannels, 0.0f),
          maxChannels_(maxChannels == 0 ? 1 : maxChannels) {}

    void setLowpass(double freqHz, double q, double sampleRate) noexcept {
        design(freqHz, q, sampleRate, /*lowpass*/ true);
    }
    void setHighpass(double freqHz, double q, double sampleRate) noexcept {
        design(freqHz, q, sampleRate, /*lowpass*/ false);
    }
    void setCoefficients(float b0, float b1, float b2, float a1, float a2) noexcept {
        b0_ = b0; b1_ = b1; b2_ = b2; a1_ = a1; a2_ = a2;
    }

    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {
        for (auto& z : z1_) z = 0.0f;
        for (auto& z : z2_) z = 0.0f;
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            float* dst = out.channel(c);
            const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
            if (c >= maxChannels_ || src == nullptr) {  // beyond our state → passthrough/silence
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src ? src[f] : 0.0f;
                continue;
            }
            float z1 = z1_[c];
            float z2 = z2_[c];
            for (std::uint32_t f = 0; f < numFrames; ++f) {
                const float x = src[f];
                const float y = b0_ * x + z1;
                z1 = b1_ * x - a1_ * y + z2;
                z2 = b2_ * x - a2_ * y;
                dst[f] = y;
            }
            z1_[c] = z1;
            z2_[c] = z2;
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "BiquadNode"; }

private:
    void design(double freqHz, double q, double sampleRate, bool lowpass) noexcept {
        constexpr double kPi = 3.14159265358979323846;
        const double w0 = 2.0 * kPi * freqHz / sampleRate;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        double b0, b1, b2;
        if (lowpass) {
            b0 = (1.0 - cw) * 0.5; b1 = 1.0 - cw; b2 = (1.0 - cw) * 0.5;
        } else {
            b0 = (1.0 + cw) * 0.5; b1 = -(1.0 + cw); b2 = (1.0 + cw) * 0.5;
        }
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 = 1.0 - alpha;
        b0_ = static_cast<float>(b0 / a0);
        b1_ = static_cast<float>(b1 / a0);
        b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(a1 / a0);
        a2_ = static_cast<float>(a2 / a0);
    }

    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    std::vector<float> z1_;
    std::vector<float> z2_;
    std::uint32_t maxChannels_;
};

}  // namespace aiudio::graph

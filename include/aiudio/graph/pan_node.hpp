// aiudio-graph — PanNode: 1 mono input → 1 stereo output, equal-power constant-power pan
// (G8 width-changing node, 1→2). pan ∈ [-1,+1] (L..R), click-free. process() is
// allocation-free (ADR-0004). For a stereo input, channel 0 is taken as the mono source.
#pragma once

#include <cmath>
#include <cstdint>

#include "aiudio/graph/node.hpp"
#include "aiudio/graph/smoothed_value.hpp"

namespace aiudio::graph {

class PanNode final : public Node {
public:
    static constexpr std::uint32_t kPan = 0;  ///< -1 = hard left, 0 = centre, +1 = hard right

    explicit PanNode(float pan = 0.0f) noexcept : panInit_(pan) {}

    void setParam(std::uint32_t index, float value) noexcept override {
        if (index == kPan) pan_.setTarget(value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value));
    }

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        pan_.prepare(sampleRate, panInit_);
    }

    // 1 input port → 1 output port that is always **stereo** (2 channels), whatever the input
    // width (G8 channel-width change).
    void channelLayout(const std::uint32_t* /*inWidths*/, std::uint32_t /*numIn*/,
                       std::uint32_t* outWidths, std::uint32_t numOut,
                       std::uint32_t /*hostChannels*/) const noexcept override {
        for (std::uint32_t o = 0; o < numOut; ++o) outWidths[o] = 2;
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        if (out.numChannels < 2) return;
        const float* src = (in.numChannels > 0) ? in.channel(0) : nullptr;
        float* l = out.channel(0);
        float* r = out.channel(1);
        constexpr float kHalfPi = 1.5707963267948966f;
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float theta = (pan_.next() * 0.5f + 0.5f) * kHalfPi;  // 0..π/2
            const float x = src ? src[f] : 0.0f;
            l[f] = x * std::cos(theta);
            r[f] = x * std::sin(theta);
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "PanNode"; }

private:
    float panInit_;
    SmoothedValue pan_;
};

}  // namespace aiudio::graph

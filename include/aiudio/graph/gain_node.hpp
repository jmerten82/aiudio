// aiudio-graph — GainNode: 1 input → 1 output, multiplies every sample by a gain.
// The simplest possible Node; a trivial classic-DSP processor for G1.
#pragma once

#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class GainNode final : public Node {
public:
    explicit GainNode(float gain = 1.0f) : gain_(gain) {}

    void setGain(float gain) noexcept { gain_ = gain; }
    [[nodiscard]] float gain() const noexcept { return gain_; }

    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    void process(const AudioBuffer* inputs, AudioBuffer* outputs,
                 std::uint32_t numFrames, const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
            float* dst = out.channel(c);
            if (src) {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src[f] * gain_;
            } else {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "GainNode"; }

private:
    float gain_;
};

}  // namespace aiudio::graph

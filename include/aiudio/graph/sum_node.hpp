// aiudio-graph — SumNode: N inputs → 1 output, sums all input ports (a mixer).
// Demonstrates a multi-input node — the reason mixing is a node, not multiple
// edges into one port (ADR-0008/0009).
#pragma once

#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class SumNode final : public Node {
public:
    explicit SumNode(std::uint32_t numInputs = 2)
        : numInputs_(numInputs == 0 ? 1 : numInputs) {}

    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    void process(const AudioBuffer* inputs, AudioBuffer* outputs,
                 std::uint32_t numFrames, const TimeInfo& /*time*/) noexcept override {
        AudioBuffer& out = outputs[0];
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            float* dst = out.channel(c);
            for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
            for (std::uint32_t i = 0; i < numInputs_; ++i) {
                const AudioBuffer& in = inputs[i];
                if (c >= in.numChannels) continue;
                const float* src = in.channel(c);
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] += src[f];
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return numInputs_; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "SumNode"; }

private:
    std::uint32_t numInputs_;
};

}  // namespace aiudio::graph

// aiudio-graph — SinkNode: 1 input → 0 outputs. The graph's exit point: writes its
// input into an externally-provided AudioBuffer that the executor sets each block
// to its `out` (G2 / ADR-0009).
#pragma once

#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class SinkNode final : public Node {
public:
    /// Set by the executor each block (points at the executor's `out`).
    void setExternalOutput(AudioBuffer* out) noexcept { external_ = out; }

    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    void process(const AudioBuffer* inputs, AudioBuffer* /*outputs*/,
                 std::uint32_t numFrames, const TimeInfo& /*time*/) noexcept override {
        if (external_ == nullptr) return;
        const AudioBuffer& in = inputs[0];
        for (std::uint32_t c = 0; c < external_->numChannels; ++c) {
            float* dst = external_->channel(c);
            if (c < in.numChannels) {
                const float* src = in.channel(c);
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src[f];
            } else {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 0; }
    [[nodiscard]] const char* typeName() const noexcept override { return "SinkNode"; }

private:
    AudioBuffer* external_ = nullptr;
};

}  // namespace aiudio::graph

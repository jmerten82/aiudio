// aiudio-graph — SourceNode: 0 inputs → 1 output. The graph's entry point: emits
// an externally-provided AudioBuffer that the executor sets each block from its
// `in` (G2 / ADR-0009). In G3 that `in` comes from a capture/duplex/tap backend.
#pragma once

#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class SourceNode final : public Node {
public:
    /// Set by the executor each block (points at the executor's `in`).
    void setExternalInput(const AudioBuffer* in) noexcept { external_ = in; }

    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    void process(const AudioBuffer* /*inputs*/, AudioBuffer* outputs,
                 std::uint32_t numFrames, const TimeInfo& /*time*/) noexcept override {
        AudioBuffer& out = outputs[0];
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            float* dst = out.channel(c);
            if (external_ != nullptr && c < external_->numChannels) {
                const float* src = external_->channel(c);
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src[f];
            } else {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 0; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "SourceNode"; }

private:
    const AudioBuffer* external_ = nullptr;
};

}  // namespace aiudio::graph

// aiudio-graph — UpmixNode: 1 input channel → N output channels (G8). Duplicates the
// (mono) input channel 0 to every output channel — a center up-mix (e.g. mono → stereo).
// The counterpart to DownmixNode: it *raises* the channel count, declared via
// channelLayout() so the executor sizes the output port to N channels.
#pragma once

#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class UpmixNode final : public Node {
public:
    explicit UpmixNode(std::uint32_t outChannels = 2) noexcept
        : outChannels_(outChannels == 0 ? 1 : outChannels) {}

    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    void channelLayout(const std::uint32_t* /*inWidths*/, std::uint32_t /*numIn*/,
                       std::uint32_t* outWidths, std::uint32_t numOut,
                       std::uint32_t /*hostChannels*/) const noexcept override {
        for (std::uint32_t o = 0; o < numOut; ++o) outWidths[o] = outChannels_;
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        const float* src = (in.numChannels > 0) ? in.channel(0) : nullptr;
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            float* dst = out.channel(c);
            if (src) {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src[f];
            } else {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "UpmixNode"; }

private:
    std::uint32_t outChannels_;
};

}  // namespace aiudio::graph

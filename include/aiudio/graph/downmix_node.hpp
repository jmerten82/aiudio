// aiudio-graph — DownmixNode: N input channels → 1 output channel (G8). Averages the
// input channels into a single mono output (level-preserving: 1/N). The first node that
// *changes* the channel count — it overrides channelLayout() to declare a 1-channel
// output, which the executor's per-port buffer sizing honors.
#pragma once

#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class DownmixNode final : public Node {
public:
    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    void channelLayout(const std::uint32_t* /*inWidths*/, std::uint32_t /*numIn*/,
                       std::uint32_t* outWidths, std::uint32_t numOut,
                       std::uint32_t /*hostChannels*/) const noexcept override {
        for (std::uint32_t o = 0; o < numOut; ++o) outWidths[o] = 1;  // always mono out
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        if (out.numChannels == 0) return;
        float* dst = out.channel(0);
        for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
        if (in.numChannels == 0) return;
        const float scale = 1.0f / static_cast<float>(in.numChannels);  // average → preserve level
        for (std::uint32_t c = 0; c < in.numChannels; ++c) {
            const float* src = in.channel(c);
            for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] += src[f] * scale;
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "DownmixNode"; }
};

}  // namespace aiudio::graph

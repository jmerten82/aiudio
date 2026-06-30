// aiudio-graph — DcBlockerNode: 1 in → 1 out one-pole DC/sub-rumble remover.
// y[n] = x[n] - x[n-1] + R·y[n-1], with R set from a ~20 Hz corner. Per-channel state;
// allocation-free process() (ADR-0004). Cheap and ubiquitous (put it after a nonlinearity).
#pragma once

#include <cstdint>
#include <vector>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class DcBlockerNode final : public Node {
public:
    explicit DcBlockerNode(double cornerHz = 20.0, std::uint32_t maxChannels = 2) noexcept
        : cornerHz_(cornerHz), maxChannels_(maxChannels == 0 ? 1 : maxChannels) {}

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        constexpr double kTwoPi = 6.283185307179586;
        double r = 1.0 - kTwoPi * cornerHz_ / (sampleRate > 0.0 ? sampleRate : 48000.0);
        r_ = static_cast<float>(r < 0.0 ? 0.0 : (r > 0.99999 ? 0.99999 : r));
        x1_.assign(maxChannels_, 0.0f);
        y1_.assign(maxChannels_, 0.0f);
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            float* dst = out.channel(c);
            const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
            if (c >= maxChannels_ || src == nullptr) {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src ? src[f] : 0.0f;
                continue;
            }
            float x1 = x1_[c], y1 = y1_[c];
            for (std::uint32_t f = 0; f < numFrames; ++f) {
                const float x = src[f];
                const float y = x - x1 + r_ * y1;
                x1 = x;
                y1 = y;
                dst[f] = y;
            }
            x1_[c] = x1;
            y1_[c] = y1;
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "DcBlockerNode"; }

private:
    double cornerHz_;
    std::uint32_t maxChannels_;
    float r_ = 0.995f;
    std::vector<float> x1_, y1_;
};

}  // namespace aiudio::graph

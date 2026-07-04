// aiudio-graph — MixerNode: N in → 1 out weighted sum (a console-style mixer). Each input
// port has its own click-free gain (SumNode has none). param index `i` = gain for input `i`.
// process() is allocation-free (ADR-0004).
#pragma once

#include <cstdint>
#include <vector>

#include "aiudio/graph/node.hpp"
#include "aiudio/graph/smoothed_value.hpp"

namespace aiudio::graph {

class MixerNode final : public Node {
public:
    /// `numInputs` input ports, each with an initial gain of `gain`.
    explicit MixerNode(std::uint32_t numInputs = 2, float gain = 1.0f)
        : numInputs_(numInputs == 0 ? 1 : numInputs), gains_(numInputs_), gainInit_(gain) {}

    /// param index = input port number → that input's gain.
    [[nodiscard]] std::vector<ParamDescriptor> paramDescriptors() const override {
        std::vector<ParamDescriptor> d;
        for (std::uint32_t i = 0; i < numInputs(); ++i)
            d.push_back({i, "gain[" + std::to_string(i) + "]", 0.0, 2.0, 1.0, "linear"});
        return d;
    }

    void setParam(std::uint32_t index, float value) noexcept override {
        if (index < numInputs_) gains_[index].setTarget(value);
    }

    [[nodiscard]] float paramValue(std::uint32_t index) const noexcept override {
        return index < numInputs_ ? gains_[index].target() : 0.0f;
    }

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        for (auto& g : gains_) g.prepare(sampleRate, gainInit_);
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        AudioBuffer& out = outputs[0];
        const std::uint32_t ch = out.numChannels;
        for (std::uint32_t c = 0; c < ch; ++c)
            for (std::uint32_t f = 0; f < numFrames; ++f) out.channel(c)[f] = 0.0f;
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            for (std::uint32_t i = 0; i < numInputs_; ++i) {
                const float g = gains_[i].next();
                const AudioBuffer& in = inputs[i];
                for (std::uint32_t c = 0; c < ch && c < in.numChannels; ++c)
                    out.channel(c)[f] += in.channel(c)[f] * g;
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return numInputs_; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "MixerNode"; }

private:
    std::uint32_t numInputs_;
    std::vector<SmoothedValue> gains_;
    float gainInit_;
};

}  // namespace aiudio::graph

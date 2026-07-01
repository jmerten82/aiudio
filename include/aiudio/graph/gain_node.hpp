// aiudio-graph — GainNode: 1 input → 1 output, multiplies every sample by a gain.
// The simplest possible Node; a trivial classic-DSP processor for G1.
#pragma once

#include <atomic>
#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class GainNode final : public Node {
public:
    /// `setParam` / `GraphExecutor::postParam` index for the gain multiplier.
    static constexpr std::uint32_t kGain = 0;

    explicit GainNode(float gain = 1.0f) : gain_(gain) {}

    // `gain_` is a relaxed atomic so a control-rate edit (applied on the audio thread
    // when the executor drains its command queue) and the audio render never form a
    // data race. The hot loop reads it once into a local, so the per-sample loop still
    // vectorizes.
    void setGain(float gain) noexcept { gain_.store(gain, std::memory_order_relaxed); }
    [[nodiscard]] float gain() const noexcept { return gain_.load(std::memory_order_relaxed); }

    void setParam(std::uint32_t index, float value) noexcept override {
        if (index == kGain) gain_.store(value, std::memory_order_relaxed);
    }

    [[nodiscard]] float paramValue(std::uint32_t index) const noexcept override {
        return index == kGain ? gain_.load(std::memory_order_relaxed) : 0.0f;
    }

    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    void process(const AudioBuffer* inputs, AudioBuffer* outputs,
                 std::uint32_t numFrames, const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        const float gain = gain_.load(std::memory_order_relaxed);
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
            float* dst = out.channel(c);
            if (src) {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src[f] * gain;
            } else {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "GainNode"; }

private:
    std::atomic<float> gain_;
};

}  // namespace aiudio::graph

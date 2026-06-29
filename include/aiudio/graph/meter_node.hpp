// aiudio-graph — MeterNode: 1 in → 1 out passthrough that measures the block's
// mean-square level into a relaxed atomic for off-thread reading (a "leaf"
// observation node). Used in the G3 live slice to show capture level.
#pragma once

#include <atomic>
#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class MeterNode final : public Node {
public:
    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    void process(const AudioBuffer* inputs, AudioBuffer* outputs,
                 std::uint32_t numFrames, const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        double sumSq = 0.0;
        std::size_t n = 0;
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
            float* dst = out.channel(c);
            for (std::uint32_t f = 0; f < numFrames; ++f) {
                const float v = src ? src[f] : 0.0f;
                dst[f] = v;  // passthrough
                sumSq += static_cast<double>(v) * v;
                ++n;
            }
        }
        meanSquare_.store(n ? static_cast<float>(sumSq / static_cast<double>(n)) : 0.0f,
                          std::memory_order_relaxed);
        calls_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "MeterNode"; }

    /// Off-thread readers (UI / probe): the most recent block's mean-square and the
    /// number of process() calls so far.
    [[nodiscard]] float meanSquare() const noexcept {
        return meanSquare_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<float> meanSquare_{0.0f};
    std::atomic<std::uint64_t> calls_{0};
};

}  // namespace aiudio::graph

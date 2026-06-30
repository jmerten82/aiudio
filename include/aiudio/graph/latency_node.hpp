// aiudio-graph — LatencyNode: passes its input through delayed by N frames AND reports
// latencyFrames()==N (G9). It models any node with inherent output latency (a lookahead
// limiter, an FFT/overlap-add block, a resampler) — used to exercise the executor's
// delay-compensation: a parallel zero-latency branch gets delayed by N so the two
// recombine in phase. (For an *intentional* delay effect you would NOT report latency.)
#pragma once

#include <cstdint>

#include "aiudio/graph/delay_line.hpp"
#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class LatencyNode final : public Node {
public:
    explicit LatencyNode(std::uint32_t frames, std::uint32_t maxChannels = 2) noexcept
        : frames_(frames), maxChannels_(maxChannels == 0 ? 1 : maxChannels) {}

    [[nodiscard]] std::uint32_t latencyFrames() const noexcept override { return frames_; }

    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {
        delay_.prepare(maxChannels_, frames_);
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        delay_.process(inputs[0], outputs[0], numFrames);
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "LatencyNode"; }

private:
    std::uint32_t frames_;
    std::uint32_t maxChannels_;
    DelayLine delay_;
};

}  // namespace aiudio::graph

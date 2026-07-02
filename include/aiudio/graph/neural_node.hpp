// aiudio-graph — NeuralNode: a first-class *neural* graph peer (Phase 1 · D7).
//
// A neural model (a torch nn.Module) is a node like any DSP node — same 1-in/1-out contract, so
// it composes in the graph and trains jointly with DSP nodes in the differentiable executor
// (ADR-0016). Its "truth" is the torch module (there's no closed-form C++ DSP to mirror), so:
//
//   - **Training / differentiable execution:** the torch module runs in `aiudio.diff`
//     (NeuralDiffNode), injected per node id.
//   - **Real-time execution (this C++ node):** a **placeholder** — RT neural inference (streaming
//     / cached-conv, RTNeural / ANIRA / LibTorch) is ADR-0006 / **Phase 3**, so `process()` is an
//     identity pass-through for now (the slot holds the topology; running the model in RT comes
//     later). `config()` reports `realtime_capable = 0` to say so honestly.
#pragma once

#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class NeuralNode final : public Node {
public:
    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    // Placeholder RT behavior: pass the input through unchanged. The trained model runs in the
    // differentiable executor (training) and, in Phase 3, via a runtime-agnostic RT backend.
    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
            float* dst = out.channel(c);
            for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src ? src[f] : 0.0f;
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "NeuralNode"; }

    /// {"neural": 1, "realtime_capable": 0} — a neural peer whose RT inference is Phase 3.
    [[nodiscard]] std::vector<std::pair<std::string, double>> config() const override {
        return {{"neural", 1.0}, {"realtime_capable", 0.0}};
    }
};

}  // namespace aiudio::graph

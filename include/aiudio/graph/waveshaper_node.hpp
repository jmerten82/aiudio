// aiudio-graph — WaveshaperNode: 1 in → 1 out nonlinear shaper (saturation / distortion).
// y = shape(drive · x), blended dry/wet. A differentiable building block (tanh/soft-clip are
// smooth) and the basis for amp/distortion modelling. drive + mix are click-free (smoothed);
// process() is allocation-free (ADR-0004).
#pragma once

#include <cmath>
#include <cstdint>

#include "aiudio/graph/node.hpp"
#include "aiudio/graph/smoothed_value.hpp"

namespace aiudio::graph {

class WaveshaperNode final : public Node {
public:
    static constexpr std::uint32_t kDrive = 0;  ///< input gain into the shaper (≥ 0)
    static constexpr std::uint32_t kMix = 1;    ///< dry/wet in [0,1]

    enum class Shape { Tanh, SoftClip, HardClip };

    explicit WaveshaperNode(Shape shape = Shape::Tanh, float drive = 1.0f, float mix = 1.0f) noexcept
        : shape_(shape), driveInit_(drive), mixInit_(mix) {}

    void setParam(std::uint32_t index, float value) noexcept override {
        if (index == kDrive) drive_.setTarget(value < 0.0f ? 0.0f : value);
        else if (index == kMix) mix_.setTarget(value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value));
    }

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        drive_.prepare(sampleRate, driveInit_);
        mix_.prepare(sampleRate, mixInit_);
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        const std::uint32_t ch = out.numChannels;
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float drive = drive_.next();
            const float mix = mix_.next();
            for (std::uint32_t c = 0; c < ch; ++c) {
                const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
                const float x = src ? src[f] : 0.0f;
                out.channel(c)[f] = x * (1.0f - mix) + shaped(x * drive) * mix;
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "WaveshaperNode"; }

private:
    [[nodiscard]] float shaped(float x) const noexcept {
        switch (shape_) {
            case Shape::Tanh: return std::tanh(x);
            case Shape::SoftClip: return x / (1.0f + std::fabs(x));  // smooth, ~[-1,1]
            case Shape::HardClip:
            default: return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
        }
    }

    Shape shape_;
    float driveInit_, mixInit_;
    SmoothedValue drive_, mix_;
};

}  // namespace aiudio::graph

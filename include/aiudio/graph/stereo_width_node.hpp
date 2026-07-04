// aiudio-graph — StereoWidthNode: 2 in → 2 out mid/side width control. mid = (L+R)/2,
// side = (L-R)/2·width, then L'=mid+side, R'=mid-side. width 0 = mono, 1 = unchanged,
// >1 = wider. width is click-free; process() is allocation-free (ADR-0004). Non-stereo
// input/output passes through.
#pragma once

#include <cstdint>

#include "aiudio/graph/node.hpp"
#include "aiudio/graph/smoothed_value.hpp"

namespace aiudio::graph {

class StereoWidthNode final : public Node {
public:
    static constexpr std::uint32_t kWidth = 0;  ///< 0 = mono … 1 = unchanged … 2 = wide

    explicit StereoWidthNode(float width = 1.0f) noexcept : widthInit_(width) {}

    [[nodiscard]] std::vector<ParamDescriptor> paramDescriptors() const override {
        return {{kWidth, "width", 0.0, 2.0, 1.0, ""}};
    }

    void setParam(std::uint32_t index, float value) noexcept override {
        if (index == kWidth) width_.setTarget(value < 0.0f ? 0.0f : value);
    }

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        width_.prepare(sampleRate, widthInit_);
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        if (out.numChannels < 2 || in.numChannels < 2) {  // not stereo → passthrough
            for (std::uint32_t c = 0; c < out.numChannels; ++c) {
                const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
                float* dst = out.channel(c);
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src ? src[f] : 0.0f;
            }
            return;
        }
        const float* L = in.channel(0);
        const float* R = in.channel(1);
        float* oL = out.channel(0);
        float* oR = out.channel(1);
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float w = width_.next();
            const float mid = (L[f] + R[f]) * 0.5f;
            const float side = (L[f] - R[f]) * 0.5f * w;
            oL[f] = mid + side;
            oR[f] = mid - side;
        }
        for (std::uint32_t c = 2; c < out.numChannels; ++c) {  // extra channels pass through
            const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
            float* dst = out.channel(c);
            for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src ? src[f] : 0.0f;
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "StereoWidthNode"; }

private:
    float widthInit_;
    SmoothedValue width_;
};

}  // namespace aiudio::graph

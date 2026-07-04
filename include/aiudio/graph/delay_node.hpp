// aiudio-graph — DelayNode: 1 in → 1 out delay effect with feedback and dry/wet mix. Feedback
// is internal (the graph is DAG-only, so the loop lives inside the node). It reports
// latencyFrames()==0: this is an *intentional* effect delay and must NOT be compensated away
// (ADR-0013). Delay buffers are allocated at prepare() up to a max delay; process() is
// allocation-free (ADR-0004). Feedback + mix are click-free; delay time is read per block.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "aiudio/graph/node.hpp"
#include "aiudio/graph/smoothed_value.hpp"

namespace aiudio::graph {

class DelayNode final : public Node {
public:
    static constexpr std::uint32_t kDelayFrames = 0;  ///< delay length in frames
    static constexpr std::uint32_t kFeedback = 1;     ///< feedback gain in [0, ~0.99]
    static constexpr std::uint32_t kMix = 2;          ///< dry/wet in [0, 1]

    explicit DelayNode(double maxDelaySeconds = 2.0, std::uint32_t delayFrames = 24000,
                       float feedback = 0.3f, float mix = 0.3f, std::uint32_t maxChannels = 2) noexcept
        : maxDelaySec_(maxDelaySeconds <= 0.0 ? 0.001 : maxDelaySeconds),
          delayInit_(delayFrames),
          fbInit_(feedback),
          mixInit_(mix),
          maxChannels_(maxChannels == 0 ? 1 : maxChannels) {}

    [[nodiscard]] std::vector<ParamDescriptor> paramDescriptors() const override {
        return {{kDelayFrames, "delay_frames", 0.0, 192000.0, 24000.0, "frames"},
                {kFeedback, "feedback", 0.0, 0.99, 0.3, ""},
                {kMix, "mix", 0.0, 1.0, 0.3, ""}};
    }

    void setParam(std::uint32_t index, float value) noexcept override {
        if (index == kDelayFrames) {
            std::uint32_t d = static_cast<std::uint32_t>(value < 0.0f ? 0.0f : value);
            if (cap_ > 0 && d > cap_ - 1) d = cap_ - 1;
            delay_.store(d, std::memory_order_relaxed);
        } else if (index == kFeedback) {
            fb_.setTarget(value < 0.0f ? 0.0f : (value > 0.99f ? 0.99f : value));
        } else if (index == kMix) {
            mix_.setTarget(value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value));
        }
    }

    [[nodiscard]] float paramValue(std::uint32_t index) const noexcept override {
        if (index == kDelayFrames) return static_cast<float>(delay_.load(std::memory_order_relaxed));
        if (index == kFeedback) return fb_.target();
        if (index == kMix) return mix_.target();
        return 0.0f;
    }

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        cap_ = static_cast<std::uint32_t>(maxDelaySec_ * (sampleRate > 0.0 ? sampleRate : 48000.0)) + 1;
        if (cap_ < 2) cap_ = 2;
        ring_.assign(maxChannels_, std::vector<float>(cap_, 0.0f));
        writePos_ = 0;
        std::uint32_t d = delayInit_;
        if (d > cap_ - 1) d = cap_ - 1;
        delay_.store(d, std::memory_order_relaxed);
        fb_.prepare(sampleRate, fbInit_);
        mix_.prepare(sampleRate, mixInit_);
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        const std::uint32_t delay = delay_.load(std::memory_order_relaxed);
        std::uint32_t w = writePos_;
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float fb = fb_.next();
            const float mix = mix_.next();
            const std::uint32_t readPos = (w + cap_ - delay) % cap_;
            for (std::uint32_t c = 0; c < out.numChannels; ++c) {
                float* dst = out.channel(c);
                const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
                const float x = src ? src[f] : 0.0f;
                if (c >= maxChannels_) { dst[f] = x; continue; }  // beyond our state → dry
                const float delayed = ring_[c][readPos];
                dst[f] = x * (1.0f - mix) + delayed * mix;
                ring_[c][w] = x + delayed * fb;  // internal feedback
            }
            w = (w + 1 == cap_) ? 0 : w + 1;
        }
        writePos_ = w;
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "DelayNode"; }

private:
    double maxDelaySec_;
    std::uint32_t delayInit_;
    float fbInit_, mixInit_;
    std::uint32_t maxChannels_;
    std::uint32_t cap_ = 0;
    std::uint32_t writePos_ = 0;
    std::atomic<std::uint32_t> delay_{0};
    SmoothedValue fb_, mix_;
    std::vector<std::vector<float>> ring_;  // [channel][cap]
};

}  // namespace aiudio::graph

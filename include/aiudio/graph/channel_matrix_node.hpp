// aiudio-graph — ChannelMatrixNode: 1 in (inCh) → 1 out (outCh), a general routing/mix matrix
// (G8 width-changing node). out[o] = Σ_i in[i]·gain[o][i] — so it can split, merge, reorder,
// or matrix-mix channels. param index = o·inCh + i → that matrix cell's gain. The matrix is
// snapshotted once per block (no per-sample atomic loads); process() is allocation-free.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class ChannelMatrixNode final : public Node {
public:
    ChannelMatrixNode(std::uint32_t inChannels, std::uint32_t outChannels)
        : inCh_(inChannels == 0 ? 1 : inChannels),
          outCh_(outChannels == 0 ? 1 : outChannels),
          gains_(static_cast<std::size_t>(inCh_) * outCh_),
          snapshot_(static_cast<std::size_t>(inCh_) * outCh_, 0.0f) {
        // Default to an identity-ish routing (out[i] = in[i] for the overlapping channels).
        for (std::uint32_t o = 0; o < outCh_; ++o)
            for (std::uint32_t i = 0; i < inCh_; ++i)
                gains_[cell(o, i)].store(o == i ? 1.0f : 0.0f, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint32_t inChannels() const noexcept { return inCh_; }
    [[nodiscard]] std::uint32_t outChannels() const noexcept { return outCh_; }

    /// param index = out·inCh + in → that cell's gain.
    void setParam(std::uint32_t index, float value) noexcept override {
        if (index < gains_.size()) gains_[index].store(value, std::memory_order_relaxed);
    }

    void prepare(double /*sampleRate*/, std::uint32_t /*maxBlock*/) override {}

    void channelLayout(const std::uint32_t* /*inWidths*/, std::uint32_t /*numIn*/,
                       std::uint32_t* outWidths, std::uint32_t numOut,
                       std::uint32_t /*hostChannels*/) const noexcept override {
        for (std::uint32_t o = 0; o < numOut; ++o) outWidths[o] = outCh_;
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        for (std::size_t k = 0; k < gains_.size(); ++k)  // snapshot the matrix for this block
            snapshot_[k] = gains_[k].load(std::memory_order_relaxed);
        const std::uint32_t outN = out.numChannels < outCh_ ? out.numChannels : outCh_;
        const std::uint32_t inN = in.numChannels < inCh_ ? in.numChannels : inCh_;
        for (std::uint32_t o = 0; o < outN; ++o) {
            float* dst = out.channel(o);
            for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
            for (std::uint32_t i = 0; i < inN; ++i) {
                const float g = snapshot_[cell(o, i)];
                if (g == 0.0f) continue;
                const float* src = in.channel(i);
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] += src[f] * g;
            }
        }
        for (std::uint32_t o = outN; o < out.numChannels; ++o) {  // unused outs → silence
            float* dst = out.channel(o);
            for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "ChannelMatrixNode"; }

private:
    [[nodiscard]] std::size_t cell(std::uint32_t o, std::uint32_t i) const noexcept {
        return static_cast<std::size_t>(o) * inCh_ + i;
    }

    std::uint32_t inCh_, outCh_;
    std::vector<std::atomic<float>> gains_;  // [out*inCh + in]
    std::vector<float> snapshot_;            // per-block copy (audio thread only)
};

}  // namespace aiudio::graph

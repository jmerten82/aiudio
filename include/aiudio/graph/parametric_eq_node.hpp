// aiudio-graph — ParametricEqNode: 1 in → 1 out multi-band EQ. A convenience node wrapping a
// cascade of `BiquadNode` sections (the band shapes — peaking / low-/high-shelf / low-/high-
// pass — already proven in BiquadNode), so a whole EQ is one node id (a clean agent /
// serialization target) instead of N chained biquads. Bands run in series, in place. State is
// allocated at prepare(); process() is allocation-free (ADR-0004); latencyFrames()==0.
//
// Live control reuses the biquad param indices, addressed per band:
//   set_param(index = band*3 + p, value), p = 0:freq(Hz)  1:Q  2:gain(dB)
#pragma once

#include <cstdint>
#include <vector>

#include "aiudio/graph/biquad_node.hpp"
#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class ParametricEqNode final : public Node {
public:
    static constexpr std::uint32_t kParamsPerBand = 3;  // freq, Q, gainDb (= biquad indices 0,1,2)

    /// One band: shape + initial design parameters (gainDb ignored for low-/high-pass).
    struct Band {
        BiquadNode::Type type = BiquadNode::Type::Peaking;
        double freqHz = 1000.0;
        double q = 0.707;
        double gainDb = 0.0;
    };

    /// `sampleRate` is the design rate (re-confirmed at prepare() from the compiled rate).
    explicit ParametricEqNode(const std::vector<Band>& bands, double sampleRate = 48000.0,
                              std::uint32_t maxChannels = 2) {
        bands_.reserve(bands.size());
        for (const Band& b : bands) {
            BiquadNode bq(maxChannels);
            switch (b.type) {
                case BiquadNode::Type::Lowpass: bq.setLowpass(b.freqHz, b.q, sampleRate); break;
                case BiquadNode::Type::Highpass: bq.setHighpass(b.freqHz, b.q, sampleRate); break;
                case BiquadNode::Type::Peaking: bq.setPeaking(b.freqHz, b.q, b.gainDb, sampleRate); break;
                case BiquadNode::Type::LowShelf: bq.setLowShelf(b.freqHz, b.q, b.gainDb, sampleRate); break;
                case BiquadNode::Type::HighShelf: bq.setHighShelf(b.freqHz, b.q, b.gainDb, sampleRate); break;
            }
            bands_.push_back(std::move(bq));
        }
    }

    [[nodiscard]] std::uint32_t numBands() const noexcept {
        return static_cast<std::uint32_t>(bands_.size());
    }

    // Route a control edit to its band's biquad: band = index / 3, biquad param = index % 3
    // (which already maps 0→cutoff, 1→Q, 2→gainDb). RT-safe (the biquad re-designs in place).
    void setParam(std::uint32_t index, float value) noexcept override {
        const std::uint32_t band = index / kParamsPerBand;
        if (band < bands_.size()) bands_[band].setParam(index % kParamsPerBand, value);
    }

    void prepare(double sampleRate, std::uint32_t maxBlock) override {
        for (auto& b : bands_) b.prepare(sampleRate, maxBlock);
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& time) noexcept override {
        if (bands_.empty()) {  // no bands → passthrough
            const AudioBuffer& in = inputs[0];
            AudioBuffer& out = outputs[0];
            for (std::uint32_t c = 0; c < out.numChannels; ++c) {
                const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
                float* dst = out.channel(c);
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src ? src[f] : 0.0f;
            }
            return;
        }
        // Band 0 reads the external input → output; later bands cascade in place on output
        // (BiquadNode reads x before writing the same index, so in-place is exact).
        bands_[0].process(inputs, outputs, numFrames, time);
        for (std::size_t i = 1; i < bands_.size(); ++i)
            bands_[i].process(outputs, outputs, numFrames, time);
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "ParametricEqNode"; }

private:
    std::vector<BiquadNode> bands_;  // cascaded sections
};

}  // namespace aiudio::graph

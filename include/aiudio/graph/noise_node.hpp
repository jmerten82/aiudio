// aiudio-graph — NoiseNode: 0 in → 1 out noise generator (white or pink). A test/excitation
// source and a DDSP filtered-noise primitive. Per-channel RNG state (xorshift32) so stereo
// channels are decorrelated; pink uses the Paul Kellet economy filter. Amplitude is
// live-controllable + click-free. process() is allocation-free (ADR-0004).
#pragma once

#include <cstdint>
#include <vector>

#include "aiudio/graph/node.hpp"
#include "aiudio/graph/smoothed_value.hpp"

namespace aiudio::graph {

class NoiseNode final : public Node {
public:
    static constexpr std::uint32_t kAmplitude = 0;

    enum class Color { White, Pink };

    explicit NoiseNode(Color color = Color::White, float amplitude = 0.5f,
                       std::uint32_t maxChannels = 2) noexcept
        : color_(color), ampInit_(amplitude), maxChannels_(maxChannels == 0 ? 1 : maxChannels) {}

    void setParam(std::uint32_t index, float value) noexcept override {
        if (index == kAmplitude) amp_.setTarget(value);
    }

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        amp_.prepare(sampleRate, ampInit_);
        rng_.assign(maxChannels_, 0x1234567u);
        for (std::uint32_t c = 0; c < maxChannels_; ++c) rng_[c] = 0x9E3779B9u * (c + 1) + 1u;
        pink_.assign(maxChannels_, Pink{});
    }

    void process(const AudioBuffer* /*inputs*/, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        AudioBuffer& out = outputs[0];
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float a = amp_.next();
            for (std::uint32_t c = 0; c < out.numChannels; ++c) {
                const std::uint32_t s = (c < maxChannels_) ? c : 0;
                const float white = nextWhite(rng_[s]);
                out.channel(c)[f] = a * (color_ == Color::Pink ? pink_[s].filter(white) : white);
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 0; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "NoiseNode"; }

private:
    // xorshift32 → white sample in [-1, 1).
    static float nextWhite(std::uint32_t& state) noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<std::int32_t>(state)) * (1.0f / 2147483648.0f);
    }

    struct Pink {
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        float filter(float w) noexcept {  // Paul Kellet's economy pink filter
            b0 = 0.99886f * b0 + w * 0.0555179f;
            b1 = 0.99332f * b1 + w * 0.0750759f;
            b2 = 0.96900f * b2 + w * 0.1538520f;
            b3 = 0.86650f * b3 + w * 0.3104856f;
            b4 = 0.55000f * b4 + w * 0.5329522f;
            b5 = -0.7616f * b5 - w * 0.0168980f;
            const float out = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f) * 0.11f;
            b6 = w * 0.115926f;
            return out;
        }
    };

    Color color_;
    float ampInit_;
    std::uint32_t maxChannels_;
    SmoothedValue amp_;
    std::vector<std::uint32_t> rng_;
    std::vector<Pink> pink_;
};

}  // namespace aiudio::graph

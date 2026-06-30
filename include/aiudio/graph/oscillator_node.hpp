// aiudio-graph — OscillatorNode: 0 in → 1 out signal generator (sine / saw / square /
// triangle). The first *source of signal* in the node library (fixes the "live output is
// silent" gap) and the differentiable DDSP synthesis primitive. Frequency + amplitude are
// live-controllable; amplitude is click-free (smoothed). The waveform is written to every
// output channel. process() is allocation-free (ADR-0004).
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "aiudio/graph/node.hpp"
#include "aiudio/graph/smoothed_value.hpp"

namespace aiudio::graph {

class OscillatorNode final : public Node {
public:
    static constexpr std::uint32_t kFrequencyHz = 0;
    static constexpr std::uint32_t kAmplitude = 1;

    enum class Waveform { Sine, Saw, Square, Triangle };

    explicit OscillatorNode(Waveform wave = Waveform::Sine, double freqHz = 440.0,
                            float amplitude = 0.5f) noexcept
        : wave_(wave), freqInit_(freqHz), ampInit_(amplitude) {}

    void setParam(std::uint32_t index, float value) noexcept override {
        if (index == kFrequencyHz) freqHz_.store(value < 0.0f ? 0.0f : value, std::memory_order_relaxed);
        else if (index == kAmplitude) amp_.setTarget(value);
    }

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        phase_ = 0.0;
        freqHz_.store(static_cast<float>(freqInit_), std::memory_order_relaxed);
        amp_.prepare(sampleRate_, ampInit_);
    }

    void process(const AudioBuffer* /*inputs*/, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        AudioBuffer& out = outputs[0];
        const double inc = static_cast<double>(freqHz_.load(std::memory_order_relaxed)) / sampleRate_;
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float s = sample(phase_) * amp_.next();
            for (std::uint32_t c = 0; c < out.numChannels; ++c) out.channel(c)[f] = s;
            phase_ += inc;
            if (phase_ >= 1.0) phase_ -= 1.0;
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 0; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "OscillatorNode"; }

private:
    [[nodiscard]] float sample(double phase) const noexcept {
        constexpr double kTwoPi = 6.283185307179586;
        switch (wave_) {
            case Waveform::Sine: return static_cast<float>(std::sin(kTwoPi * phase));
            case Waveform::Saw: return static_cast<float>(2.0 * phase - 1.0);
            case Waveform::Square: return phase < 0.5 ? 1.0f : -1.0f;
            case Waveform::Triangle:
            default: return static_cast<float>(4.0 * std::fabs(phase - 0.5) - 1.0);
        }
    }

    Waveform wave_;
    double freqInit_;
    float ampInit_;
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
    std::atomic<float> freqHz_{440.0f};
    SmoothedValue amp_;
};

}  // namespace aiudio::graph

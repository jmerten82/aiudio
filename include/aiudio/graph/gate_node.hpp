// aiudio-graph — GateNode: 1 in → 1 out noise gate / downward expander. A linked peak
// detector opens the gate (gain → 1) when the level is above threshold and closes it (gain →
// the `range` floor) below, smoothed by attack (opening) / release (closing). process() is
// allocation-free (ADR-0004); no look-ahead (latencyFrames()==0).
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class GateNode final : public Node {
public:
    static constexpr std::uint32_t kThresholdDb = 0;  ///< open above this level
    static constexpr std::uint32_t kAttackMs = 1;     ///< how fast the gate opens
    static constexpr std::uint32_t kReleaseMs = 2;    ///< how fast it closes
    static constexpr std::uint32_t kRangeDb = 3;      ///< attenuation when closed (e.g. -80 dB)

    explicit GateNode(float thresholdDb = -45.0f, float attackMs = 1.0f, float releaseMs = 120.0f,
                      float rangeDb = -80.0f) noexcept {
        thresholdDb_.store(thresholdDb, std::memory_order_relaxed);
        attackMs_.store(attackMs, std::memory_order_relaxed);
        releaseMs_.store(releaseMs, std::memory_order_relaxed);
        rangeDb_.store(rangeDb, std::memory_order_relaxed);
    }

    void setParam(std::uint32_t index, float value) noexcept override {
        switch (index) {
            case kThresholdDb: thresholdDb_.store(value, std::memory_order_relaxed); break;
            case kAttackMs: attackMs_.store(value < 0.0f ? 0.0f : value, std::memory_order_relaxed); break;
            case kReleaseMs: releaseMs_.store(value < 0.0f ? 0.0f : value, std::memory_order_relaxed); break;
            case kRangeDb: rangeDb_.store(value, std::memory_order_relaxed); break;
            default: break;
        }
    }

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        env_ = 1.0f;
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        const std::uint32_t ch = out.numChannels;
        const float threshDb = thresholdDb_.load(std::memory_order_relaxed);
        const float floorGain = dbToLin(rangeDb_.load(std::memory_order_relaxed));
        const float atkCoeff = coeff(attackMs_.load(std::memory_order_relaxed));
        const float relCoeff = coeff(releaseMs_.load(std::memory_order_relaxed));

        for (std::uint32_t f = 0; f < numFrames; ++f) {
            float peak = 0.0f;
            for (std::uint32_t c = 0; c < ch && c < in.numChannels; ++c) {
                const float a = std::fabs(in.channel(c)[f]);
                if (a > peak) peak = a;
            }
            const float levelDb = 20.0f * std::log10(peak + 1e-9f);
            const float targetGain = (levelDb >= threshDb) ? 1.0f : floorGain;
            const float envCoeff = (targetGain > env_) ? atkCoeff : relCoeff;  // opening vs closing
            env_ += (targetGain - env_) * envCoeff;
            for (std::uint32_t c = 0; c < ch; ++c) {
                const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
                out.channel(c)[f] = (src ? src[f] : 0.0f) * env_;
            }
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "GateNode"; }

private:
    static float dbToLin(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
    [[nodiscard]] float coeff(float ms) const noexcept {
        const double t = static_cast<double>(ms) * 0.001 * sampleRate_;
        return t < 1.0 ? 1.0f : static_cast<float>(1.0 - std::exp(-1.0 / t));
    }

    double sampleRate_ = 48000.0;
    float env_ = 1.0f;
    std::atomic<float> thresholdDb_{-45.0f}, attackMs_{1.0f}, releaseMs_{120.0f}, rangeDb_{-80.0f};
};

}  // namespace aiudio::graph

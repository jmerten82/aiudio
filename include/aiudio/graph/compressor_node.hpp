// aiudio-graph — CompressorNode: 1 in → 1 out downward compressor / limiter with optional
// look-ahead. A linked (stereo-summed) peak detector drives a dB gain computer
// (threshold/ratio), smoothed by attack/release; makeup gain is applied after. With
// lookahead > 0 the signal is delayed by that many frames while the detector reads the
// undelayed input, so the gain reduction starts *before* a transient — and the node reports
// latencyFrames()==lookahead so the executor delay-compensates parallel paths (G9). A high
// ratio makes it a brickwall-ish limiter. process() is allocation-free (ADR-0004).
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

class CompressorNode final : public Node {
public:
    static constexpr std::uint32_t kThresholdDb = 0;  ///< level above which gain reduction starts
    static constexpr std::uint32_t kRatio = 1;        ///< ≥ 1 (∞-ish = limiter)
    static constexpr std::uint32_t kAttackMs = 2;
    static constexpr std::uint32_t kReleaseMs = 3;
    static constexpr std::uint32_t kMakeupDb = 4;

    explicit CompressorNode(float thresholdDb = -18.0f, float ratio = 4.0f, float attackMs = 5.0f,
                            float releaseMs = 80.0f, std::uint32_t lookaheadFrames = 0,
                            std::uint32_t maxChannels = 2) noexcept
        : lookahead_(lookaheadFrames), maxChannels_(maxChannels == 0 ? 1 : maxChannels) {
        thresholdDb_.store(thresholdDb, std::memory_order_relaxed);
        ratio_.store(ratio < 1.0f ? 1.0f : ratio, std::memory_order_relaxed);
        attackMs_.store(attackMs, std::memory_order_relaxed);
        releaseMs_.store(releaseMs, std::memory_order_relaxed);
        makeupDb_.store(0.0f, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint32_t latencyFrames() const noexcept override { return lookahead_; }

    [[nodiscard]] std::vector<ParamDescriptor> paramDescriptors() const override {
        return {{kThresholdDb, "threshold_db", -60.0, 0.0, -18.0, "dB"},
                {kRatio, "ratio", 1.0, 20.0, 4.0, "ratio"},
                {kAttackMs, "attack_ms", 0.0, 200.0, 5.0, "ms"},
                {kReleaseMs, "release_ms", 0.0, 1000.0, 80.0, "ms"},
                {kMakeupDb, "makeup_db", 0.0, 24.0, 0.0, "dB"}};
    }

    void setParam(std::uint32_t index, float value) noexcept override {
        switch (index) {
            case kThresholdDb: thresholdDb_.store(value, std::memory_order_relaxed); break;
            case kRatio: ratio_.store(value < 1.0f ? 1.0f : value, std::memory_order_relaxed); break;
            case kAttackMs: attackMs_.store(value < 0.0f ? 0.0f : value, std::memory_order_relaxed); break;
            case kReleaseMs: releaseMs_.store(value < 0.0f ? 0.0f : value, std::memory_order_relaxed); break;
            case kMakeupDb: makeupDb_.store(value, std::memory_order_relaxed); break;
            default: break;
        }
    }

    [[nodiscard]] float paramValue(std::uint32_t index) const noexcept override {
        switch (index) {
            case kThresholdDb: return thresholdDb_.load(std::memory_order_relaxed);
            case kRatio: return ratio_.load(std::memory_order_relaxed);
            case kAttackMs: return attackMs_.load(std::memory_order_relaxed);
            case kReleaseMs: return releaseMs_.load(std::memory_order_relaxed);
            case kMakeupDb: return makeupDb_.load(std::memory_order_relaxed);
            default: return 0.0f;
        }
    }

    void prepare(double sampleRate, std::uint32_t /*maxBlock*/) override {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        env_ = 1.0f;  // current linear gain (1 = no reduction)
        la_.assign(maxChannels_, std::vector<float>(lookahead_ + 1, 0.0f));
        laPos_ = 0;
    }

    void process(const AudioBuffer* inputs, AudioBuffer* outputs, std::uint32_t numFrames,
                 const TimeInfo& /*time*/) noexcept override {
        const AudioBuffer& in = inputs[0];
        AudioBuffer& out = outputs[0];
        const std::uint32_t ch = out.numChannels;
        const float threshDb = thresholdDb_.load(std::memory_order_relaxed);
        const float ratio = ratio_.load(std::memory_order_relaxed);
        const float slope = 1.0f - 1.0f / ratio;  // dB of reduction per dB over threshold
        const float makeup = dbToLin(makeupDb_.load(std::memory_order_relaxed));
        const float atkCoeff = coeff(attackMs_.load(std::memory_order_relaxed));
        const float relCoeff = coeff(releaseMs_.load(std::memory_order_relaxed));

        for (std::uint32_t f = 0; f < numFrames; ++f) {
            // Linked peak detector over the (undelayed) input.
            float peak = 0.0f;
            for (std::uint32_t c = 0; c < ch && c < in.numChannels; ++c) {
                const float a = std::fabs(in.channel(c)[f]);
                if (a > peak) peak = a;
            }
            const float levelDb = 20.0f * std::log10(peak + 1e-9f);
            float targetGain = 1.0f;
            if (levelDb > threshDb) targetGain = dbToLin(-slope * (levelDb - threshDb));
            // Attack when clamping down (target < env), release when recovering.
            const float envCoeff = (targetGain < env_) ? atkCoeff : relCoeff;
            env_ += (targetGain - env_) * envCoeff;
            const float g = env_ * makeup;

            for (std::uint32_t c2 = 0; c2 < ch; ++c2) {
                const float* src = (c2 < in.numChannels) ? in.channel(c2) : nullptr;
                const float x = src ? src[f] : 0.0f;
                float delayed = x;
                if (c2 < maxChannels_ && lookahead_ > 0) {  // delay the signal by `lookahead_`
                    delayed = la_[c2][laPos_];
                    la_[c2][laPos_] = x;
                }
                out.channel(c2)[f] = delayed * g;
            }
            if (lookahead_ > 0) laPos_ = (laPos_ + 1 == lookahead_ + 1) ? 0 : laPos_ + 1;
        }
    }

    [[nodiscard]] std::uint32_t numInputs() const noexcept override { return 1; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept override { return 1; }
    [[nodiscard]] const char* typeName() const noexcept override { return "CompressorNode"; }

private:
    static float dbToLin(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
    [[nodiscard]] float coeff(float ms) const noexcept {
        const double t = static_cast<double>(ms) * 0.001 * sampleRate_;
        return t < 1.0 ? 1.0f : static_cast<float>(1.0 - std::exp(-1.0 / t));
    }

    std::uint32_t lookahead_;
    std::uint32_t maxChannels_;
    double sampleRate_ = 48000.0;
    float env_ = 1.0f;  // audio thread only
    std::atomic<float> thresholdDb_{-18.0f}, ratio_{4.0f}, attackMs_{5.0f}, releaseMs_{80.0f}, makeupDb_{0.0f};
    std::vector<std::vector<float>> la_;  // [channel][lookahead+1] look-ahead delay
    std::uint32_t laPos_ = 0;
};

}  // namespace aiudio::graph

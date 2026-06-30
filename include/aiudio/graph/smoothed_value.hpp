// aiudio-graph — SmoothedValue: a click-free control parameter. A control-rate target is set
// off the audio thread (atomic, relaxed) via setTarget(); the audio thread ramps the current
// value toward it one sample at a time (a one-pole glide), so live parameter edits don't
// produce zipper noise. RT-safe: next() is allocation-/lock-free. Used by the effect nodes'
// continuous gain-like parameters (drive, mix, pan, width, per-input gains).
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace aiudio::graph {

class SmoothedValue {
public:
    /// Set the smoothing time constant + initial value (jumps current straight to `initial`).
    /// `timeMs` is the ~63% glide time. Setup-time.
    void prepare(double sampleRate, float initial, double timeMs = 8.0) noexcept {
        double tc = (timeMs <= 0.0 ? 1.0 : timeMs) * 0.001 * sampleRate;
        if (tc < 1.0) tc = 1.0;
        coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / tc));
        current_ = initial;
        target_.store(initial, std::memory_order_relaxed);
    }

    /// Control thread: set the value the audio thread should glide toward. RT-safe.
    void setTarget(float t) noexcept { target_.store(t, std::memory_order_relaxed); }
    [[nodiscard]] float target() const noexcept { return target_.load(std::memory_order_relaxed); }

    /// Snap current straight to a value (no glide) — e.g. at prepare/reset.
    void snap(float v) noexcept {
        current_ = v;
        target_.store(v, std::memory_order_relaxed);
    }

    /// Audio thread: advance one sample toward the target and return the smoothed value.
    float next() noexcept {
        const float t = target_.load(std::memory_order_relaxed);
        current_ += (t - current_) * coeff_;
        return current_;
    }

    [[nodiscard]] float current() const noexcept { return current_; }

private:
    std::atomic<float> target_{0.0f};
    float current_ = 0.0f;  // audio thread only
    float coeff_ = 1.0f;
};

}  // namespace aiudio::graph

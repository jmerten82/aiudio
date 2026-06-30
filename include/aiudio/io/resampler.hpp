// aiudio-io — Resampler (M9.3): a streaming, real-time-safe fractional sample-rate
// converter for planar float32 audio. It sits at the I/O *boundary* — between a device
// running at one clock/rate and the engine running at another (ADR-0015) — never as a
// graph node (the graph is single-rate; resampling changes the rate, so it lives at the
// edge). A 4-tap Catmull-Rom (cubic Hermite) interpolator gives smooth, alloc-free
// conversion; `setRatio()` lets the ratio vary sample-accurately at run time, which is
// exactly what the drift-compensation control loop (M9.5) drives.
//
// Model: `ratio = inputRate / outputRate` = input frames consumed per output frame.
//   ratio < 1  → upsampling   (e.g. 44100/48000 ≈ 0.91875, more output than input)
//   ratio > 1  → downsampling (e.g. 48000/44100 ≈ 1.08844, less output than input)
// `process()` is *pull*-shaped: it produces up to `outCap` output frames, consuming up to
// `inAvail` input frames, and reports how many of each it used — so a caller pulling a
// fixed engine block keeps the leftover input for next time. State (the 4-sample window +
// fractional phase) carries across calls, so block boundaries are seamless.
#pragma once

#include <cstdint>
#include <vector>

namespace aiudio::io {

class Resampler {
public:
    /// Widest/narrowest ratio supported (1/16× … 16×). The drift loop nudges within this.
    static constexpr double kMinRatio = 1.0 / 16.0;
    static constexpr double kMaxRatio = 16.0;

    /// Allocate per-channel interpolation state for `channels` channels at the given
    /// `ratio` (= inputRate/outputRate). Setup-time (allocates); resets all state.
    void prepare(std::uint32_t channels, double ratio) {
        channels_ = channels == 0 ? 1 : channels;
        win_.assign(static_cast<std::size_t>(channels_) * kTaps, 0.0f);
        setRatio(ratio);
        reset();
    }

    /// Clear the interpolation window + phase without reallocating (e.g. after a reconnect).
    /// `phase_ = 1` forces the first output to pull the first input sample in. RT-safe.
    void reset() noexcept {
        for (auto& s : win_) s = 0.0f;
        phase_ = 1.0;
    }

    /// Change the conversion ratio (clamped to [kMinRatio, kMaxRatio]). RT-safe — the
    /// drift-compensation loop (M9.5) calls this every block to track a clock offset.
    void setRatio(double ratio) noexcept {
        ratio_ = ratio < kMinRatio ? kMinRatio : (ratio > kMaxRatio ? kMaxRatio : ratio);
    }

    [[nodiscard]] double ratio() const noexcept { return ratio_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

    /// Group delay of the interpolation kernel, in OUTPUT frames — the lag between a
    /// transient at the input and its peak at the output. Used to report boundary latency
    /// to the executor's delay compensation (G9). ~2 input samples of kernel delay,
    /// expressed at the output rate.
    [[nodiscard]] std::uint32_t latencyFrames() const noexcept {
        const double f = static_cast<double>(kTaps) / 2.0 / ratio_;  // 2 input samples / ratio
        return static_cast<std::uint32_t>(f + 0.5);
    }

    struct Result {
        std::uint32_t consumed;  ///< input frames read
        std::uint32_t produced;  ///< output frames written
    };

    /// Resample: produce up to `outCap` output frames from up to `inAvail` input frames.
    /// Both are planar — `in[c][0..inAvail)`, `out[c][0..produced)` — with `channels()`
    /// rows expected. Stops early when input runs out (the caller refills and calls again)
    /// or `outCap` is reached. Stateful across calls. RT-safe (no allocation/locks).
    Result process(const float* const* in, std::uint32_t inAvail, float* const* out,
                   std::uint32_t outCap) noexcept {
        std::uint32_t produced = 0;
        std::uint32_t consumed = 0;
        while (produced < outCap) {
            // Advance the window until the read point lies in the current [x1, x2) interval.
            while (phase_ >= 1.0) {
                if (consumed >= inAvail) {  // out of input — return what we have
                    return {consumed, produced};
                }
                for (std::uint32_t c = 0; c < channels_; ++c) {
                    float* w = &win_[static_cast<std::size_t>(c) * kTaps];
                    w[0] = w[1];
                    w[1] = w[2];
                    w[2] = w[3];
                    w[3] = in[c][consumed];  // newest sample
                }
                ++consumed;
                phase_ -= 1.0;
            }
            const float t = static_cast<float>(phase_);
            for (std::uint32_t c = 0; c < channels_; ++c) {
                const float* w = &win_[static_cast<std::size_t>(c) * kTaps];
                out[c][produced] = catmull(w[0], w[1], w[2], w[3], t);
            }
            ++produced;
            phase_ += ratio_;
        }
        return {consumed, produced};
    }

private:
    static constexpr std::uint32_t kTaps = 4;

    /// Catmull-Rom (cubic Hermite) interpolation at fractional position `t` ∈ [0,1) within
    /// the interval [p1, p2]; p0/p3 set the tangents. t=0 → p1, t=1 → p2. Smooth C1.
    static float catmull(float p0, float p1, float p2, float p3, float t) noexcept {
        const float a0 = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
        const float a1 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        const float a2 = -0.5f * p0 + 0.5f * p2;
        return ((a0 * t + a1) * t + a2) * t + p1;
    }

    std::uint32_t channels_ = 1;
    double ratio_ = 1.0;
    double phase_ = 1.0;          // fractional read position in [0,1); 1 forces a load
    std::vector<float> win_;      // [channel*kTaps + tap]; tap 3 is newest
};

}  // namespace aiudio::io

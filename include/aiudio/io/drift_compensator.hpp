// aiudio-io — DriftCompensator (M9.5): the ring-fill control loop that makes boundary
// resampling *adaptive* (ADR-0015 §3). Two devices on separate physical clocks drift by
// tens of PPM even at the same nominal rate, so a producer feeding a consumer at the
// "same" rate slowly over- or under-fills the ring between them → an eventual xrun. This
// servo watches that ring's fill level and nudges the `Resampler` ratio so the fill stays
// centered: too full → consume faster (ratio up); too empty → consume slower (ratio down).
//
// It is a classic proportional ASRC servo — it needs no knowledge of the true clock ratio,
// only the observable fill error. df/dt = inRate − ratio·outRate, and with
// ratio = nominal·(1 + gain·e) (e = normalized fill error) the loop is first-order stable:
// at steady state consumeRate == inRate exactly, leaving only a tiny, bounded fill offset
// (≈ driftFraction/gain) — far inside the ring. RT-safe: update() is wait-free.
#pragma once

namespace aiudio::io {

class DriftCompensator {
public:
    struct Config {
        double targetFill = 0.0;     ///< desired ring fill in frames (e.g. ringCapacity/2)
        double gain = 0.05;          ///< proportional gain on the normalized fill error
        double maxDeviation = 0.05;  ///< max fractional ratio deviation from nominal (±5%)
        double slew = 0.25;          ///< per-update smoothing of ratio changes in (0,1]
    };

    /// Set the nominal (no-drift) ratio = sourceRate/engineRate and the loop config.
    /// Setup-time; no allocation. The corrected ratio starts at nominal.
    void prepare(double nominalRatio, const Config& cfg) noexcept {
        nominal_ = nominalRatio;
        cfg_ = cfg;
        if (cfg_.targetFill <= 0.0) cfg_.targetFill = 1.0;  // guard /0
        if (cfg_.slew <= 0.0 || cfg_.slew > 1.0) cfg_.slew = 1.0;
        ratio_ = nominalRatio;
    }

    /// One control step: given the current input-side fill (frames buffered ahead of the
    /// engine), return the resample ratio to apply this block. RT-safe.
    double update(double currentFill) noexcept {
        const double err = (currentFill - cfg_.targetFill) / cfg_.targetFill;  // normalized
        double target = nominal_ * (1.0 + cfg_.gain * err);
        const double lo = nominal_ * (1.0 - cfg_.maxDeviation);
        const double hi = nominal_ * (1.0 + cfg_.maxDeviation);
        if (target < lo) target = lo;
        if (target > hi) target = hi;
        ratio_ += cfg_.slew * (target - ratio_);  // slew-limit toward the target ratio
        return ratio_;
    }

    [[nodiscard]] double ratio() const noexcept { return ratio_; }
    [[nodiscard]] double nominalRatio() const noexcept { return nominal_; }
    void reset() noexcept { ratio_ = nominal_; }

private:
    Config cfg_{};
    double nominal_ = 1.0;
    double ratio_ = 1.0;
};

}  // namespace aiudio::io

// aiudio-io — ResamplingSource (M9.5): brings an off-clock source onto the engine timeline
// (ADR-0008 "aggregate-then-resample"; ADR-0015). It bundles the three pieces that a
// drift-compensated boundary needs into one reusable unit:
//   • a per-channel lock-free SPSC ring (the thread boundary, ADR-0008 §2),
//   • a `Resampler` (fractional rate conversion at the edge, M9.3),
//   • a `DriftCompensator` (the ring-fill servo that adapts the ratio, M9.5).
//
// The producer (an off-clock device IOProc, a file thread, Python) calls push() at its own
// rate; the engine (the master clock) calls pull() for a fixed block at the engine rate. On
// each pull the servo reads the ring fill, nudges the resampler ratio, and converts ring
// audio → engine-rate output — so a slowly drifting source stays sample-aligned and the ring
// never runs dry or overflows. Both push() and pull() are wait-free (ADR-0004); only
// prepare() allocates. This is exactly the unit the cross-clock multi-device path (M9.6)
// wires between a device and the MultiSourceManager.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/drift_compensator.hpp"
#include "aiudio/io/resampler.hpp"
#include "aiudio/io/ring_buffer.hpp"

namespace aiudio::io {

class ResamplingSource {
public:
    ResamplingSource() = default;

    // Owns lock-free rings (unique_ptr) → non-copyable (also keeps nanobind from generating
    // an ill-formed copy ctor, the trap MultiSourceManager/Graph hit).
    ResamplingSource(const ResamplingSource&) = delete;
    ResamplingSource& operator=(const ResamplingSource&) = delete;

    /// Allocate for `channels` channels converting `nominalRatio` = sourceRate/engineRate,
    /// with a `ringFrames`-deep per-channel ring (in *source*-rate frames) and engine pulls
    /// up to `maxBlock` frames. The drift servo targets a half-full ring. Setup-time
    /// (allocates); `cfg` overrides the servo defaults (targetFill is auto-set if 0).
    void prepare(std::uint32_t channels, double nominalRatio, std::uint32_t ringFrames,
                 std::uint32_t maxBlock, DriftCompensator::Config cfg = {});

    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

    /// Producer (source thread): push `srcFrames` of planar source-rate audio into the rings.
    /// Returns frames accepted (< srcFrames → ring full / overrun, counted). RT-safe.
    std::uint32_t push(const AudioBuffer& src, std::uint32_t srcFrames) noexcept;

    /// Engine (master clock): pull `engineFrames` of planar engine-rate audio. Updates the
    /// drift ratio from the current fill, resamples from the rings, silence-fills any
    /// underrun. Returns engineFrames (always — the rest is silence on underrun). RT-safe.
    std::uint32_t pull(AudioBuffer& dst, std::uint32_t engineFrames) noexcept;

    // ---- telemetry (atomic → safe to read from a control/monitor thread, ADR-0010) ----
    [[nodiscard]] double ratio() const noexcept { return ratioTel_.load(std::memory_order_relaxed); }
    [[nodiscard]] double nominalRatio() const noexcept { return drift_.nominalRatio(); }
    /// Source-rate frames currently buffered ahead of the engine (ring + staging).
    [[nodiscard]] std::uint32_t fillFrames() const noexcept {
        return fillTel_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t overruns() const noexcept;  // producer couldn't write (ring atomic)
    [[nodiscard]] std::uint64_t underruns() const noexcept {  // engine starved the resampler
        return underruns_.load(std::memory_order_relaxed);
    }

private:
    std::uint32_t channels_ = 0;
    std::uint32_t maxBlock_ = 0;
    std::uint32_t stageCap_ = 0;  // capacity of each staging buffer (source frames)
    std::uint32_t staged_ = 0;    // unconsumed source frames staged (engine thread only)

    // Cross-thread telemetry: written by the engine thread in pull(), read anywhere.
    std::atomic<float> ratioTel_{1.0f};
    std::atomic<std::uint32_t> fillTel_{0};
    std::atomic<std::uint64_t> underruns_{0};

    std::vector<std::unique_ptr<RingBuffer<float>>> rings_;  // [channel]
    std::vector<std::vector<float>> stage_;                  // [channel][stageCap]
    std::vector<const float*> stagePtrs_;                    // resampler input views
    std::vector<std::vector<float>> outScratch_;             // [channel][maxBlock] resampler out
    std::vector<float*> outScratchPtrs_;                     // resampler output views
    std::vector<float> zero_;                                // silence (missing src channels)

    Resampler resampler_;
    DriftCompensator drift_;
};

}  // namespace aiudio::io

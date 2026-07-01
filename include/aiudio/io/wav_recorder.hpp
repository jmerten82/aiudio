// aiudio-io — WavRecorder: record a planar block stream to a WAV **off the audio thread**
// (ADR-0004). The audio thread pushes each output block into a lock-free SPSC ring
// (wait-free, alloc-free); a dedicated writer thread drains the ring and calls
// WavWriter::write() (blocking file I/O). The audio thread NEVER signals the writer (that
// would be a syscall) — the writer polls; the audio thread only ever pushes.
//
// Lifecycle (all of start()/stop() are control-thread, non-RT):
//   start(path, …)         open the WAV + spawn the writer thread + arm pushBlock()
//   pushBlock(block, n)    RT thread: interleave → ring.write (drops+counts if full)
//   stop()                 disarm, writer does a final drain, join, finalize the WAV
//
// A full ring (disk stalling longer than `ringFrames`) drops blocks and counts them
// (droppedFrames) — the RT-safe degradation; it can't block the audio thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/ring_buffer.hpp"
#include "aiudio/io/wav_file.hpp"

namespace aiudio::io {

class WavRecorder {
public:
    WavRecorder() = default;
    ~WavRecorder() { stop(); }
    WavRecorder(const WavRecorder&) = delete;
    WavRecorder& operator=(const WavRecorder&) = delete;

    /// Open `path` and start the writer thread. `ringFrames` = ring depth in frames (buffer
    /// against disk stalls; ~1 s at engine rate is a sane default). `maxBlock` bounds the
    /// per-push frame count (the RT interleave scratch is sized for it). Returns false if the
    /// file couldn't be created. Control-thread; allocates. No-op-false if already recording.
    bool start(const std::string& path, std::uint32_t channels, double sampleRate,
               WavFormat format = WavFormat::Float32, std::uint32_t ringFrames = 48000,
               std::uint32_t maxBlock = 8192);

    /// RT thread: enqueue one planar (channels, frames) block. Wait-free, alloc-free; drops
    /// (and counts) if the ring is full. A no-op while not recording.
    void pushBlock(const AudioBuffer& block, std::uint32_t frames) noexcept;

    /// Control-thread: stop recording — writer does a final drain, then join + finalize the
    /// WAV. Idempotent (safe to call twice, and called by the destructor).
    void stop();

    [[nodiscard]] bool recording() const noexcept {
        return active_.load(std::memory_order_acquire);
    }
    /// Frames actually written to the file (writer thread; telemetry).
    [[nodiscard]] std::uint64_t framesWritten() const noexcept {
        return framesWritten_.load(std::memory_order_relaxed);
    }
    /// Frames dropped because the ring was full (disk not keeping up). ~0 on healthy systems.
    [[nodiscard]] std::uint64_t droppedFrames() const noexcept;

private:
    void writerLoop();

    std::unique_ptr<WavWriter> writer_;
    std::unique_ptr<RingBuffer<float>> ring_;
    std::thread thread_;
    std::atomic<bool> active_{false};        // pushBlock enqueues only while true
    std::atomic<bool> stopRequested_{false};
    std::atomic<std::uint64_t> framesWritten_{0};

    std::uint32_t channels_ = 0;
    std::uint32_t chunkFrames_ = 0;  // writer drains up to this many frames per iteration

    std::vector<float> interleave_;  // RT-thread (producer) scratch: planar → frame-major
    std::vector<float> drain_;       // writer-thread (consumer) scratch: ring → frame-major
    std::vector<float> planar_;      // writer-thread scratch: frame-major → planar for write()
    std::vector<float*> planarPtrs_;
};

}  // namespace aiudio::io

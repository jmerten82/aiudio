// aiudio-io — a clock/transport that drives a RenderCallback.
//
// Backends are the "swappable clock" of ADR-0005: a Core Audio device backend
// (output IOProc is the clock), a plugin-host backend (the host calls us), or an
// offline backend (a manual pump). enumerate()/open() are setup-time and may
// allocate; start()/stop() bound the real-time lifetime.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "aiudio/io/types.hpp"

namespace aiudio::io {

class RenderCallback;

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    /// List available devices (setup-time; not real-time-safe).
    virtual std::vector<AudioDeviceInfo> enumerate() = 0;

    /// Open a stream with `config`, routing each block to `callback`.
    /// Returns false on failure. Not real-time-safe.
    virtual bool open(const StreamConfig& config, RenderCallback* callback) = 0;

    /// Begin invoking the callback on the real-time thread.
    virtual bool start() = 0;

    /// Stop invoking the callback and release the stream.
    virtual void stop() = 0;

    /// Total round-trip latency in frames, for delay compensation upstream.
    [[nodiscard]] virtual std::uint32_t latencyFrames() const = 0;

    // ---- Hot-plug + xrun model (M9.4) ----
    /// Called (off the audio thread) when the backend detects its device went away — a
    /// real backend fires it from a device-died listener; a consumer reacts (fall back /
    /// re-open / notify the UI). Set before start(). NOT invoked on the audio thread.
    using DisconnectHandler = std::function<void()>;
    void setDisconnectHandler(DisconnectHandler handler) { onDisconnect_ = std::move(handler); }

    /// True once the device has gone away (hot-unplug / death). Telemetry, off-thread.
    [[nodiscard]] bool disconnected() const noexcept {
        return disconnected_.load(std::memory_order_acquire);
    }
    /// Device-side xruns detected (e.g. a sample-time discontinuity / overload). The
    /// engine-side xrun counter is on GraphExecutor (M9.1); this is what only the device
    /// can see.
    [[nodiscard]] std::uint64_t xrunCount() const noexcept {
        return xruns_.load(std::memory_order_acquire);
    }

protected:
    /// Backends call this when the device disconnects: marks the state and fires the
    /// handler (on the listener/control thread, never the audio thread).
    void notifyDisconnect() {
        const bool was = disconnected_.exchange(true, std::memory_order_acq_rel);
        if (!was && onDisconnect_) onDisconnect_();
    }
    /// Audio thread: record `n` xrun frames (e.g. a detected sample-time gap).
    void addXruns(std::uint64_t n) noexcept { xruns_.fetch_add(n, std::memory_order_release); }

    std::atomic<bool> disconnected_{false};
    std::atomic<std::uint64_t> xruns_{0};
    DisconnectHandler onDisconnect_;
};

}  // namespace aiudio::io

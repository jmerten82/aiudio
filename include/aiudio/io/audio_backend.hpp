// aiudio-io — a clock/transport that drives a RenderCallback.
//
// Backends are the "swappable clock" of ADR-0005: a Core Audio device backend
// (output IOProc is the clock), a plugin-host backend (the host calls us), or an
// offline backend (a manual pump). enumerate()/open() are setup-time and may
// allocate; start()/stop() bound the real-time lifetime.
#pragma once

#include <cstdint>
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
};

}  // namespace aiudio::io

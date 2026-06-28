// aiudio-io — the single real-time entry point the graph executor implements.
//
// One duplex callback, driven by a swappable clock/backend (ADR-0005). Input and
// output meet here. Implementations MUST be real-time-safe (ADR-0004): no heap
// allocation, no locks, no syscalls, no logging, no exceptions, no unbounded work.
#pragma once

#include <cstdint>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/types.hpp"

namespace aiudio::io {

class RenderCallback {
public:
    virtual ~RenderCallback() = default;

    /// Render one block. `in` carries captured audio (may be empty for
    /// output-only clocks); fill `out` with `numFrames` frames (may be empty for
    /// input-only clocks). Must not block or allocate.
    virtual void process(const AudioBuffer& in, AudioBuffer& out,
                         std::uint32_t numFrames, const TimeInfo& time) noexcept = 0;
};

}  // namespace aiudio::io

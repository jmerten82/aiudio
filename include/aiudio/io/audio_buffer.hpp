// aiudio-io — non-owning view of deinterleaved (planar) float32 audio.
//
// RT-safe by construction (ADR-0004): holds only pointers + sizes, owns nothing,
// allocates nothing. This is the buffer type that crosses the RenderCallback
// boundary (ADR-0005).
#pragma once

#include <cstdint>

namespace aiudio::io {

/// A non-owning view over planar float32 channels: `channels[ch][frame]`.
/// Either side of a duplex callback may have `numChannels == 0`.
struct AudioBuffer {
    float* const* channels = nullptr;  ///< array of `numChannels` channel pointers
    std::uint32_t numChannels = 0;
    std::uint32_t numFrames = 0;

    /// Pointer to a single channel's samples. Caller guarantees `c < numChannels`.
    [[nodiscard]] float* channel(std::uint32_t c) const noexcept { return channels[c]; }

    [[nodiscard]] bool empty() const noexcept {
        return numChannels == 0 || numFrames == 0;
    }
};

}  // namespace aiudio::io

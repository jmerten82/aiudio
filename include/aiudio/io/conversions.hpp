// aiudio-io — sample-format conversions between device buffers and the engine.
//
// Devices/hosts hand us interleaved and/or integer samples; the engine works in
// planar float32. These helpers are the boundary. All are real-time-safe
// (allocation-free, noexcept) and operate in place over caller-owned memory.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aiudio::io {

/// Planar (`planar[ch][frame]`) -> interleaved (`[f*ch + c]`).
void interleave(const float* const* planar, float* interleaved,
                std::uint32_t numChannels, std::uint32_t numFrames) noexcept;

/// Interleaved (`[f*ch + c]`) -> planar (`planar[ch][frame]`).
void deinterleave(const float* interleaved, float* const* planar,
                  std::uint32_t numChannels, std::uint32_t numFrames) noexcept;

/// int16 PCM (full-scale ±32768) -> float32 (full-scale ±1.0).
void int16ToFloat(const std::int16_t* src, float* dst, std::size_t count) noexcept;

/// float32 (±1.0) -> int16 PCM, clamping out-of-range values to avoid wrap.
void floatToInt16(const float* src, std::int16_t* dst, std::size_t count) noexcept;

}  // namespace aiudio::io

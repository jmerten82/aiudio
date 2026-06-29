#include "aiudio/io/conversions.hpp"

#include <algorithm>

namespace aiudio::io {

void interleave(const float* const* planar, float* interleaved,
                std::uint32_t numChannels, std::uint32_t numFrames) noexcept {
    for (std::uint32_t f = 0; f < numFrames; ++f) {
        for (std::uint32_t c = 0; c < numChannels; ++c) {
            interleaved[f * numChannels + c] = planar[c][f];
        }
    }
}

void deinterleave(const float* interleaved, float* const* planar,
                  std::uint32_t numChannels, std::uint32_t numFrames) noexcept {
    for (std::uint32_t f = 0; f < numFrames; ++f) {
        for (std::uint32_t c = 0; c < numChannels; ++c) {
            planar[c][f] = interleaved[f * numChannels + c];
        }
    }
}

void int16ToFloat(const std::int16_t* src, float* dst, std::size_t count) noexcept {
    constexpr float kScale = 1.0f / 32768.0f;
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = static_cast<float>(src[i]) * kScale;
    }
}

void floatToInt16(const float* src, std::int16_t* dst, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        const float v = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<std::int16_t>(v * 32767.0f);
    }
}

}  // namespace aiudio::io

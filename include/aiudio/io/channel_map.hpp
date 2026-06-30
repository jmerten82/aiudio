// aiudio-io — channel mapping at the I/O boundary (M9.2). A device's channel count rarely
// equals the graph's: a mono mic feeds a stereo graph; a stereo graph monitors to a mono
// speaker. The graph's own DownmixNode/UpmixNode (G8) handle *within-graph* width changes;
// this is the complementary piece — mapping device layout ↔ graph layout right at the edge,
// where there is no node. It is a small, allocation-free, RT-safe planar copy (ADR-0004).
//
// Modes:
//   Auto           — the natural mono/stereo behavior: 1→N duplicates the mono channel to
//                    all outputs, N→1 averages all inputs, N→M copies min(N,M) and zero-pads.
//   Copy           — copy min(N,M) channels, zero-pad any extra destination channels.
//   DuplicateMono  — copy source channel 0 to every destination channel.
//   DownmixToMono  — average all source channels into destination channel 0 (rest zeroed).
#pragma once

#include <cstdint>

#include "aiudio/io/audio_buffer.hpp"

namespace aiudio::io {

enum class ChannelMapMode { Auto, Copy, DuplicateMono, DownmixToMono };

/// Map `frames` of planar audio from `src`'s channel layout to `dst`'s. RT-safe (no
/// allocation/locks). `dst` channels not written by the chosen mode are zero-filled.
inline void mapChannels(const AudioBuffer& src, AudioBuffer& dst, std::uint32_t frames,
                        ChannelMapMode mode = ChannelMapMode::Auto) noexcept {
    const std::uint32_t sc = src.numChannels;
    const std::uint32_t dc = dst.numChannels;
    if (sc == 0 || dc == 0) {
        for (std::uint32_t c = 0; c < dc; ++c) {
            float* d = dst.channel(c);
            for (std::uint32_t f = 0; f < frames; ++f) d[f] = 0.0f;
        }
        return;
    }

    if (mode == ChannelMapMode::Auto) {
        if (sc == 1 && dc > 1)
            mode = ChannelMapMode::DuplicateMono;
        else if (dc == 1 && sc > 1)
            mode = ChannelMapMode::DownmixToMono;
        else
            mode = ChannelMapMode::Copy;
    }

    switch (mode) {
        case ChannelMapMode::DuplicateMono: {
            const float* s = src.channel(0);
            for (std::uint32_t c = 0; c < dc; ++c) {
                float* d = dst.channel(c);
                for (std::uint32_t f = 0; f < frames; ++f) d[f] = s[f];
            }
            break;
        }
        case ChannelMapMode::DownmixToMono: {
            float* d0 = dst.channel(0);
            const float inv = 1.0f / static_cast<float>(sc);
            for (std::uint32_t f = 0; f < frames; ++f) {
                float acc = 0.0f;
                for (std::uint32_t c = 0; c < sc; ++c) acc += src.channel(c)[f];
                d0[f] = acc * inv;
            }
            for (std::uint32_t c = 1; c < dc; ++c) {  // any extra dst channels → silence
                float* d = dst.channel(c);
                for (std::uint32_t f = 0; f < frames; ++f) d[f] = 0.0f;
            }
            break;
        }
        case ChannelMapMode::Copy:
        case ChannelMapMode::Auto: {  // Auto already resolved; listed for completeness
            const std::uint32_t n = sc < dc ? sc : dc;
            for (std::uint32_t c = 0; c < n; ++c) {
                const float* s = src.channel(c);
                float* d = dst.channel(c);
                for (std::uint32_t f = 0; f < frames; ++f) d[f] = s[f];
            }
            for (std::uint32_t c = n; c < dc; ++c) {  // extra dst channels → silence
                float* d = dst.channel(c);
                for (std::uint32_t f = 0; f < frames; ++f) d[f] = 0.0f;
            }
            break;
        }
    }
}

}  // namespace aiudio::io

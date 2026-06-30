// aiudio-graph — DelayLine: a fixed integer-sample delay for a planar multichannel
// block (G9). One per-channel ring of `delay` frames; output[f] is the sample that
// entered `delay` frames earlier. Storage is allocated at prepare(); process() is
// real-time-safe (no allocation/locks). Used both by LatencyNode and by the executor's
// delay-compensation (PDC) machinery.
#pragma once

#include <cstdint>
#include <vector>

#include "aiudio/io/audio_buffer.hpp"

namespace aiudio::graph {

class DelayLine {
public:
    /// Allocate for `channels` channels and a `delay`-frame delay. Setup-time (allocates).
    void prepare(std::uint32_t channels, std::uint32_t delay) {
        channels_ = channels;
        delay_ = delay;
        writePos_ = 0;
        ring_.assign(channels, std::vector<float>(delay == 0 ? 0 : delay, 0.0f));
    }

    [[nodiscard]] std::uint32_t delay() const noexcept { return delay_; }

    /// Write `in` through the delay into `out` (both planar, `frames` long). delay==0 is a
    /// straight copy. RT-safe.
    void process(const io::AudioBuffer& in, io::AudioBuffer& out, std::uint32_t frames) noexcept {
        const std::uint32_t ch = out.numChannels < channels_ ? out.numChannels : channels_;
        if (delay_ == 0) {
            for (std::uint32_t c = 0; c < out.numChannels; ++c) {
                float* dst = out.channel(c);
                const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
                for (std::uint32_t f = 0; f < frames; ++f) dst[f] = src ? src[f] : 0.0f;
            }
            return;
        }
        std::uint32_t endPos = writePos_;
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            float* dst = out.channel(c);
            if (c >= ch) {  // beyond delayed channels → silence
                for (std::uint32_t f = 0; f < frames; ++f) dst[f] = 0.0f;
                continue;
            }
            float* ring = ring_[c].data();
            const float* src = (c < in.numChannels) ? in.channel(c) : nullptr;
            std::uint32_t w = writePos_;
            for (std::uint32_t f = 0; f < frames; ++f) {
                dst[f] = ring[w];                       // sample from `delay` frames ago
                ring[w] = src ? src[f] : 0.0f;          // write the newest
                w = (w + 1 == delay_) ? 0 : w + 1;
            }
            endPos = w;
        }
        writePos_ = endPos;
    }

private:
    std::uint32_t channels_ = 0;
    std::uint32_t delay_ = 0;
    std::uint32_t writePos_ = 0;
    std::vector<std::vector<float>> ring_;  // [channel][delay]
};

}  // namespace aiudio::graph

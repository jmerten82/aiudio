#include "aiudio/graph/multi_source_manager.hpp"

#include <cstddef>

namespace aiudio::graph {

MultiSourceManager::MultiSourceManager(std::uint32_t numInputs, std::uint32_t numOutputs,
                                       std::uint32_t channels, std::uint32_t maxBlock,
                                       std::uint32_t ringFrames)
    : numIn_(numInputs),
      numOut_(numOutputs),
      channels_(channels == 0 ? 1 : channels),
      maxBlock_(maxBlock == 0 ? 1 : maxBlock) {
    zero_.assign(maxBlock_, 0.0f);
    const std::size_t ringCap = ringFrames == 0 ? maxBlock_ : ringFrames;

    auto build = [&](std::uint32_t nStreams, std::vector<std::unique_ptr<io::RingBuffer<float>>>& rings,
                     std::vector<std::vector<float>>& store, std::vector<std::vector<float*>>& ptrs,
                     std::vector<io::AudioBuffer>& bufs) {
        rings.reserve(static_cast<std::size_t>(nStreams) * channels_);
        store.reserve(nStreams);
        ptrs.reserve(nStreams);
        bufs.reserve(nStreams);
        for (std::uint32_t s = 0; s < nStreams; ++s) {
            for (std::uint32_t c = 0; c < channels_; ++c)
                rings.push_back(std::make_unique<io::RingBuffer<float>>(ringCap));
            store.emplace_back(static_cast<std::size_t>(channels_) * maxBlock_, 0.0f);
            ptrs.emplace_back(channels_, nullptr);
            for (std::uint32_t c = 0; c < channels_; ++c)
                ptrs[s][c] = store[s].data() + static_cast<std::size_t>(c) * maxBlock_;
            bufs.push_back(io::AudioBuffer{ptrs[s].data(), channels_, maxBlock_});
        }
    };
    build(numIn_, inRings_, inStore_, inPtrs_, inBufs_);
    build(numOut_, outRings_, outStore_, outPtrs_, outBufs_);
}

std::uint32_t MultiSourceManager::pushInput(std::uint32_t stream, const io::AudioBuffer& src,
                                            std::uint32_t frames) noexcept {
    if (stream >= numIn_) return 0;
    if (frames > maxBlock_) frames = maxBlock_;
    std::uint32_t accepted = frames;
    for (std::uint32_t c = 0; c < channels_; ++c) {
        const float* s = (c < src.numChannels) ? src.channel(c) : zero_.data();
        const std::uint32_t n =
            static_cast<std::uint32_t>(inRings_[stream * channels_ + c]->write(s, frames));
        if (n < accepted) accepted = n;
    }
    return accepted;
}

std::uint32_t MultiSourceManager::popOutput(std::uint32_t stream, io::AudioBuffer& dst,
                                            std::uint32_t frames) noexcept {
    if (stream >= numOut_) return 0;
    if (frames > maxBlock_) frames = maxBlock_;
    std::uint32_t produced = frames;
    for (std::uint32_t c = 0; c < channels_ && c < dst.numChannels; ++c) {
        float* d = dst.channel(c);
        const std::uint32_t n =
            static_cast<std::uint32_t>(outRings_[stream * channels_ + c]->read(d, frames));
        for (std::uint32_t f = n; f < frames; ++f) d[f] = 0.0f;  // underrun → silence
        if (n < produced) produced = n;
    }
    return produced;
}

void MultiSourceManager::process(GraphExecutor& exec, std::uint32_t frames,
                                 const io::TimeInfo& time) noexcept {
    if (frames > maxBlock_) frames = maxBlock_;

    // Drain each input ring into its planar buffer; underrun → silence.
    for (std::uint32_t s = 0; s < numIn_; ++s)
        for (std::uint32_t c = 0; c < channels_; ++c) {
            float* dst = inBufs_[s].channel(c);
            const std::uint32_t n =
                static_cast<std::uint32_t>(inRings_[s * channels_ + c]->read(dst, frames));
            for (std::uint32_t f = n; f < frames; ++f) dst[f] = 0.0f;
        }

    exec.process(inBufs_.data(), numIn_, outBufs_.data(), numOut_, frames, time);

    // Push each output buffer to its ring; overrun (sink not draining) → drop (counted).
    for (std::uint32_t s = 0; s < numOut_; ++s)
        for (std::uint32_t c = 0; c < channels_; ++c)
            outRings_[s * channels_ + c]->write(outBufs_[s].channel(c), frames);
}

std::uint64_t MultiSourceManager::inputUnderruns(std::uint32_t stream) const noexcept {
    return (stream < numIn_) ? inRings_[stream * channels_]->underrunCount() : 0;
}

std::uint64_t MultiSourceManager::outputOverruns(std::uint32_t stream) const noexcept {
    return (stream < numOut_) ? outRings_[stream * channels_]->overrunCount() : 0;
}

}  // namespace aiudio::graph

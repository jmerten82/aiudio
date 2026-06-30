#include "aiudio/io/resampling_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace aiudio::io {

void ResamplingSource::prepare(std::uint32_t channels, double nominalRatio,
                               std::uint32_t ringFrames, std::uint32_t maxBlock,
                               DriftCompensator::Config cfg) {
    channels_ = channels == 0 ? 1 : channels;
    maxBlock_ = maxBlock == 0 ? 1 : maxBlock;
    const std::uint32_t ringCap = ringFrames < maxBlock_ ? maxBlock_ : ringFrames;

    // Servo targets a half-full ring by default (frames are source-rate).
    if (cfg.targetFill <= 0.0) cfg.targetFill = ringCap * 0.5;
    drift_.prepare(nominalRatio, cfg);
    resampler_.prepare(channels_, nominalRatio);

    // Worst-case input pulled per block = engineFrames · maxRatio + kernel/fractional guard.
    const double maxRatio = nominalRatio * (1.0 + cfg.maxDeviation);
    stageCap_ = static_cast<std::uint32_t>(
                    std::ceil(static_cast<double>(maxBlock_) * maxRatio)) +
                32;
    staged_ = 0;
    underruns_.store(0, std::memory_order_relaxed);
    fillTel_.store(0, std::memory_order_relaxed);
    ratioTel_.store(static_cast<float>(nominalRatio), std::memory_order_relaxed);

    rings_.clear();
    rings_.reserve(channels_);
    for (std::uint32_t c = 0; c < channels_; ++c)
        rings_.push_back(std::make_unique<RingBuffer<float>>(ringCap));

    stage_.assign(channels_, std::vector<float>(stageCap_, 0.0f));
    stagePtrs_.assign(channels_, nullptr);
    outScratch_.assign(channels_, std::vector<float>(maxBlock_, 0.0f));
    outScratchPtrs_.assign(channels_, nullptr);
    for (std::uint32_t c = 0; c < channels_; ++c) {
        stagePtrs_[c] = stage_[c].data();
        outScratchPtrs_[c] = outScratch_[c].data();
    }
    zero_.assign(ringCap, 0.0f);
}

std::uint32_t ResamplingSource::push(const AudioBuffer& src, std::uint32_t srcFrames) noexcept {
    if (srcFrames > zero_.size()) srcFrames = static_cast<std::uint32_t>(zero_.size());
    std::uint32_t accepted = srcFrames;
    for (std::uint32_t c = 0; c < channels_; ++c) {
        const float* s = (c < src.numChannels) ? src.channel(c) : zero_.data();
        const auto n = static_cast<std::uint32_t>(rings_[c]->write(s, srcFrames));
        if (n < accepted) accepted = n;
    }
    return accepted;
}

std::uint32_t ResamplingSource::pull(AudioBuffer& dst, std::uint32_t engineFrames) noexcept {
    if (engineFrames > maxBlock_) engineFrames = maxBlock_;

    // 1. Adapt the ratio from the current input-side fill (ring + already-staged).
    const auto ringFill = static_cast<std::uint32_t>(rings_[0]->sizeApprox());
    const double ratio = drift_.update(static_cast<double>(staged_ + ringFill));
    resampler_.setRatio(ratio);

    // 2. Stage enough source frames to produce engineFrames (ring stays lock-step across
    //    channels because push() writes all channels together; read channel 0 first, then
    //    the rest by the same count to preserve alignment).
    const auto want = static_cast<std::uint32_t>(static_cast<double>(engineFrames) * ratio) + 8;
    const std::uint32_t room = stageCap_ - staged_;
    std::uint32_t toRead = std::min(want, room);
    toRead = std::min(toRead, ringFill);
    if (toRead > 0) {
        const auto got =
            static_cast<std::uint32_t>(rings_[0]->read(stage_[0].data() + staged_, toRead));
        for (std::uint32_t c = 1; c < channels_; ++c)
            rings_[c]->read(stage_[c].data() + staged_, got);
        staged_ += got;
    }

    // 3. Resample staged input → engine-rate output (into the channel-complete scratch).
    const auto r = resampler_.process(stagePtrs_.data(), staged_, outScratchPtrs_.data(),
                                      engineFrames);

    // 4. Shift the unconsumed tail to the front of each staging buffer.
    if (r.consumed > 0 && r.consumed < staged_)
        for (std::uint32_t c = 0; c < channels_; ++c)
            std::copy(stage_[c].data() + r.consumed, stage_[c].data() + staged_,
                      stage_[c].data());
    staged_ -= r.consumed;

    // 5. Copy to dst (min channels), silence-pad an underrun and any extra dst channels.
    const std::uint32_t ch = std::min(channels_, dst.numChannels);
    for (std::uint32_t c = 0; c < ch; ++c) {
        float* d = dst.channel(c);
        for (std::uint32_t f = 0; f < r.produced; ++f) d[f] = outScratch_[c][f];
        for (std::uint32_t f = r.produced; f < engineFrames; ++f) d[f] = 0.0f;
    }
    for (std::uint32_t c = ch; c < dst.numChannels; ++c) {
        float* d = dst.channel(c);
        for (std::uint32_t f = 0; f < engineFrames; ++f) d[f] = 0.0f;
    }
    if (r.produced < engineFrames)
        underruns_.fetch_add(engineFrames - r.produced, std::memory_order_relaxed);

    // Publish telemetry for any monitor thread (atomic, ADR-0010).
    ratioTel_.store(static_cast<float>(ratio), std::memory_order_relaxed);
    fillTel_.store(staged_ + static_cast<std::uint32_t>(rings_[0]->sizeApprox()),
                   std::memory_order_relaxed);
    return engineFrames;
}

std::uint64_t ResamplingSource::overruns() const noexcept { return rings_[0]->overrunCount(); }

}  // namespace aiudio::io

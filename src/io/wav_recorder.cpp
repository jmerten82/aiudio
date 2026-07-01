#include "aiudio/io/wav_recorder.hpp"

#include <algorithm>
#include <chrono>

namespace aiudio::io {

bool WavRecorder::start(const std::string& path, std::uint32_t channels, double sampleRate,
                        WavFormat format, std::uint32_t ringFrames, std::uint32_t maxBlock) {
    if (active_.load(std::memory_order_acquire)) return false;  // already recording
    channels_ = channels == 0 ? 1 : channels;
    chunkFrames_ = std::max<std::uint32_t>(maxBlock == 0 ? 1 : maxBlock, 4096);
    if (ringFrames == 0) ringFrames = chunkFrames_;

    writer_ = std::make_unique<WavWriter>(path, channels_, sampleRate, format);
    if (!writer_->ok()) {  // couldn't open the file
        writer_.reset();
        return false;
    }
    ring_ = std::make_unique<RingBuffer<float>>(static_cast<std::size_t>(channels_) * ringFrames);
    const std::size_t scratch = static_cast<std::size_t>(channels_) * chunkFrames_;
    interleave_.assign(scratch, 0.0f);
    drain_.assign(scratch, 0.0f);
    planar_.assign(scratch, 0.0f);
    planarPtrs_.assign(channels_, nullptr);

    stopRequested_.store(false, std::memory_order_relaxed);
    framesWritten_.store(0, std::memory_order_relaxed);
    thread_ = std::thread([this] { writerLoop(); });
    active_.store(true, std::memory_order_release);  // arm pushBlock only once all is ready
    return true;
}

void WavRecorder::pushBlock(const AudioBuffer& block, std::uint32_t frames) noexcept {
    if (!active_.load(std::memory_order_acquire) || ring_ == nullptr) return;
    const std::uint32_t ch = channels_;
    if (static_cast<std::size_t>(frames) * ch > interleave_.size())
        frames = static_cast<std::uint32_t>(interleave_.size() / ch);  // clamp (should not trip)
    // Planar → frame-major interleave into the producer scratch, then one wait-free ring write.
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < ch; ++c) {
            const float v = (c < block.numChannels) ? block.channel(c)[f] : 0.0f;
            interleave_[static_cast<std::size_t>(f) * ch + c] = v;
        }
    }
    ring_->write(interleave_.data(), static_cast<std::size_t>(frames) * ch);
}

void WavRecorder::writerLoop() {
    using namespace std::chrono_literals;
    const std::uint32_t ch = channels_;
    for (;;) {
        const bool stopping = stopRequested_.load(std::memory_order_acquire);
        const std::size_t framesAvail = ring_->sizeApprox() / ch;
        if (framesAvail == 0) {
            if (stopping) break;              // armed to stop and nothing left → done
            std::this_thread::sleep_for(2ms);  // non-RT poll; never signaled by the audio thread
            continue;
        }
        const auto f = static_cast<std::uint32_t>(std::min<std::size_t>(framesAvail, chunkFrames_));
        const std::size_t got = ring_->read(drain_.data(), static_cast<std::size_t>(f) * ch);
        const auto gotFrames = static_cast<std::uint32_t>(got / ch);
        // Frame-major → planar, then one blocking file write (off the audio thread).
        for (std::uint32_t c = 0; c < ch; ++c) {
            float* dst = planar_.data() + static_cast<std::size_t>(c) * chunkFrames_;
            for (std::uint32_t i = 0; i < gotFrames; ++i)
                dst[i] = drain_[static_cast<std::size_t>(i) * ch + c];
            planarPtrs_[c] = dst;
        }
        writer_->write(planarPtrs_.data(), ch, gotFrames);
        framesWritten_.fetch_add(gotFrames, std::memory_order_relaxed);
    }
}

void WavRecorder::stop() {
    if (!thread_.joinable() && !active_.load(std::memory_order_acquire)) return;  // never/already
    active_.store(false, std::memory_order_release);        // producer stops enqueuing
    stopRequested_.store(true, std::memory_order_release);  // writer: final drain then exit
    if (thread_.joinable()) thread_.join();
    if (writer_) writer_->finalize();  // idempotent; the dtor would also do it
}

std::uint64_t WavRecorder::droppedFrames() const noexcept {
    if (ring_ == nullptr || channels_ == 0) return 0;
    return ring_->overrunCount() / channels_;
}

}  // namespace aiudio::io

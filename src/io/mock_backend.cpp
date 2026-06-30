#include "aiudio/io/mock_backend.hpp"

#include "aiudio/io/render_callback.hpp"

namespace aiudio::io {

std::vector<AudioDeviceInfo> MockBackend::enumerate() {
    AudioDeviceInfo d;
    d.id = "mock";
    d.name = "Mock Device";
    d.inputChannels = 2;
    d.outputChannels = 2;
    d.sampleRates = {48000.0};
    d.isDefaultInput = true;
    d.isDefaultOutput = true;
    return {d};
}

bool MockBackend::open(const StreamConfig& config, RenderCallback* callback) {
    if (callback == nullptr) return false;
    cb_ = callback;
    inCh_ = config.inputChannels;
    outCh_ = config.outputChannels;
    maxFrames_ = config.blockSize == 0 ? 1 : config.blockSize;
    sampleRate_ = config.sampleRate;
    inStore_.assign(inCh_, std::vector<float>(maxFrames_, 0.0f));
    outStore_.assign(outCh_, std::vector<float>(maxFrames_, 0.0f));
    inPtrs_.resize(inCh_);
    outPtrs_.resize(outCh_);
    for (std::uint32_t c = 0; c < inCh_; ++c) inPtrs_[c] = inStore_[c].data();
    for (std::uint32_t c = 0; c < outCh_; ++c) outPtrs_[c] = outStore_[c].data();
    opened_ = true;
    return true;
}

bool MockBackend::start() {
    if (!opened_ || disconnected()) return false;
    running_ = true;
    return true;
}

void MockBackend::stop() { running_ = false; }

void MockBackend::tick(std::uint32_t frames) {
    if (!running_ || disconnected() || cb_ == nullptr) return;
    if (frames > maxFrames_) frames = maxFrames_;
    for (std::uint32_t c = 0; c < inCh_; ++c)
        for (std::uint32_t f = 0; f < frames; ++f) inStore_[c][f] = inputValue_;
    for (std::uint32_t c = 0; c < outCh_; ++c)
        for (std::uint32_t f = 0; f < frames; ++f) outStore_[c][f] = 0.0f;
    AudioBuffer in{inPtrs_.data(), inCh_, frames};
    AudioBuffer out{outPtrs_.data(), outCh_, frames};
    const TimeInfo time{sampleTime_, static_cast<double>(sampleTime_) / sampleRate_, true};
    cb_->process(in, out, frames, time);
    sampleTime_ += frames;
}

void MockBackend::injectDisconnect() {
    running_ = false;
    notifyDisconnect();  // fires the handler (control thread) + sets disconnected()
}

void MockBackend::injectReconnect() noexcept { disconnected_.store(false, std::memory_order_release); }

float MockBackend::capturedOutput(std::uint32_t channel) const noexcept {
    return (channel < outCh_ && !outStore_[channel].empty()) ? outStore_[channel][0] : 0.0f;
}

}  // namespace aiudio::io

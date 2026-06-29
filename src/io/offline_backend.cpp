#include "aiudio/io/offline_backend.hpp"

#include <utility>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"

namespace aiudio::io {

OfflineBackend::OfflineBackend(std::string inputWavPath, std::string outputWavPath,
                               WavFormat outputFormat)
    : outputPath_(std::move(outputWavPath)),
      outputFormat_(outputFormat),
      reader_(inputWavPath) {}

std::uint32_t OfflineBackend::inputChannels() const noexcept { return reader_.channels(); }
double OfflineBackend::inputSampleRate() const noexcept { return reader_.sampleRate(); }
std::uint64_t OfflineBackend::inputFrames() const noexcept { return reader_.totalFrames(); }
bool OfflineBackend::inputOk() const noexcept { return reader_.ok(); }

bool OfflineBackend::open(const StreamConfig& config, RenderCallback* callback) {
    if (!reader_.ok() || callback == nullptr) return false;
    callback_ = callback;
    channels_ = reader_.channels();
    blockSize_ = (config.blockSize > 0) ? config.blockSize : 128;

    inStore_.assign(channels_, std::vector<float>(blockSize_, 0.0f));
    outStore_.assign(channels_, std::vector<float>(blockSize_, 0.0f));
    inPtrs_.assign(channels_, nullptr);
    outPtrs_.assign(channels_, nullptr);
    for (std::uint32_t c = 0; c < channels_; ++c) {
        inPtrs_[c] = inStore_[c].data();
        outPtrs_[c] = outStore_[c].data();
    }
    return true;
}

bool OfflineBackend::start() {
    if (callback_ == nullptr || channels_ == 0) return false;
    WavWriter writer(outputPath_, channels_, reader_.sampleRate(), outputFormat_);
    if (!writer.ok()) return false;

    framesRendered_ = 0;
    std::uint64_t sampleTime = 0;
    const double sr = reader_.sampleRate();
    for (;;) {
        const std::uint32_t got = reader_.read(inPtrs_.data(), channels_, blockSize_);
        if (got == 0) break;

        const AudioBuffer in{inPtrs_.data(), channels_, got};
        AudioBuffer out{outPtrs_.data(), channels_, got};
        const TimeInfo time{sampleTime, sr > 0 ? static_cast<double>(sampleTime) / sr : 0.0, true};
        callback_->process(in, out, got, time);

        writer.write(outPtrs_.data(), channels_, got);
        framesRendered_ += got;
        sampleTime += got;
    }
    writer.finalize();
    return true;
}

}  // namespace aiudio::io

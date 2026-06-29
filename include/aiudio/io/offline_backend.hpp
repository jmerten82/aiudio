// aiudio-io — OfflineBackend (M6): the offline "clock" of ADR-0005. It drives a
// RenderCallback over a WAV file, block by block, **faster than real time** —
// reading the input file into `in` and writing the callback's `out` to an output
// file. The SAME RenderCallback (e.g. a GraphExecutor) that runs live runs here
// unchanged. start() renders synchronously to completion.
//
// Cross-platform (pure file I/O — no Core Audio).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aiudio/io/audio_backend.hpp"
#include "aiudio/io/wav_file.hpp"

namespace aiudio::io {

class RenderCallback;

class OfflineBackend final : public AudioBackend {
public:
    OfflineBackend(std::string inputWavPath, std::string outputWavPath,
                   WavFormat outputFormat = WavFormat::Float32);

    // The input file's format (available after construction; use these to compile
    // a graph executor with matching channels / sample rate).
    [[nodiscard]] std::uint32_t inputChannels() const noexcept;
    [[nodiscard]] double inputSampleRate() const noexcept;
    [[nodiscard]] std::uint64_t inputFrames() const noexcept;
    [[nodiscard]] bool inputOk() const noexcept;

    std::vector<AudioDeviceInfo> enumerate() override { return {}; }
    bool open(const StreamConfig& config, RenderCallback* callback) override;
    bool start() override;  // renders input -> callback -> output to completion
    void stop() override {}
    [[nodiscard]] std::uint32_t latencyFrames() const override { return 0; }

    [[nodiscard]] std::uint64_t framesRendered() const noexcept { return framesRendered_; }

private:
    std::string outputPath_;
    WavFormat outputFormat_;
    WavReader reader_;
    RenderCallback* callback_ = nullptr;
    std::uint32_t channels_ = 0;
    std::uint32_t blockSize_ = 128;
    std::uint64_t framesRendered_ = 0;

    // Pre-allocated planar in/out buffers (sized at open()).
    std::vector<std::vector<float>> inStore_;
    std::vector<std::vector<float>> outStore_;
    std::vector<float*> inPtrs_;
    std::vector<float*> outPtrs_;
};

}  // namespace aiudio::io

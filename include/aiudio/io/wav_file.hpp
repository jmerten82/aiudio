// aiudio-io — minimal WAV file reader/writer (M6). Supports canonical PCM-16 and
// 32-bit IEEE-float WAV, mono/multi-channel, with a planar float32 block API that
// matches the engine's buffer convention (docs/73). Not real-time (file I/O); used
// by the offline backend and tooling. (Broader formats via AVFoundation/libsndfile
// are a future enhancement.)
#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace aiudio::io {

enum class WavFormat { Int16, Float32 };

class WavReader {
public:
    explicit WavReader(const std::string& path);

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] std::uint64_t totalFrames() const noexcept { return totalFrames_; }

    /// Read up to `maxFrames` frames into `planar[ch][frame]` (planar float32,
    /// ±1.0). Returns the number of frames actually read (0 at end of data).
    std::uint32_t read(float* const* planar, std::uint32_t channels, std::uint32_t maxFrames);

private:
    std::ifstream file_;
    bool ok_ = false;
    std::uint16_t formatTag_ = 0;  // 1 = PCM, 3 = IEEE float
    std::uint16_t bits_ = 0;
    std::uint32_t channels_ = 0;
    double sampleRate_ = 0.0;
    std::uint64_t totalFrames_ = 0;
    std::uint64_t framesRead_ = 0;
};

class WavWriter {
public:
    WavWriter(const std::string& path, std::uint32_t channels, double sampleRate,
              WavFormat format = WavFormat::Int16);

    [[nodiscard]] bool ok() const noexcept { return file_.good(); }

    /// Write `frames` from `planar[ch][frame]` (planar float32, ±1.0).
    void write(const float* const* planar, std::uint32_t channels, std::uint32_t frames);

    /// Patch the RIFF/data sizes and flush. Safe to call once after writing.
    void finalize();

private:
    void putU32(std::uint32_t v);
    void putU16(std::uint16_t v);

    std::ofstream file_;
    WavFormat format_;
    std::uint32_t channels_;
    std::uint64_t dataBytes_ = 0;
    bool finalized_ = false;
};

}  // namespace aiudio::io

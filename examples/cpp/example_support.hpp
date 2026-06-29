// Reusable pieces for the aiudio-io C++ examples: a couple of concrete
// RenderCallback "nodes", an offline "manual pump" that drives them without a
// device (the ADR-0005 offline clock), and a minimal 16-bit WAV writer.
//
// These live in examples/ — they are teaching code, not part of the library.
#pragma once

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"

namespace aiudio::examples {

inline constexpr double kPi = 3.14159265358979323846;

// A stateful sine source: ignores its input and fills every output channel with
// a phase-continuous sine. Shows the simplest RenderCallback (output-only).
class SineSource final : public io::RenderCallback {
public:
    SineSource(double freqHz, double sampleRate, float amplitude = 0.2f)
        : freq_(freqHz), sampleRate_(sampleRate), amp_(amplitude) {}

    void process(const io::AudioBuffer& /*in*/, io::AudioBuffer& out,
                 std::uint32_t numFrames, const io::TimeInfo& /*t*/) noexcept override {
        const double step = 2.0 * kPi * freq_ / sampleRate_;
        for (std::uint32_t f = 0; f < numFrames; ++f) {
            const float s = amp_ * static_cast<float>(std::sin(phase_));
            for (std::uint32_t c = 0; c < out.numChannels; ++c) out.channel(c)[f] = s;
            phase_ += step;
            if (phase_ >= 2.0 * kPi) phase_ -= 2.0 * kPi;
        }
    }

private:
    double freq_;
    double sampleRate_;
    float amp_;
    double phase_ = 0.0;
};

// Copies input to output with a linear gain — a minimal duplex node honoring the
// node contract. Extra output channels are filled from input channel 0; with no
// input, output is silence.
class GainNode final : public io::RenderCallback {
public:
    explicit GainNode(float gain) : gain_(gain) {}

    void process(const io::AudioBuffer& in, io::AudioBuffer& out,
                 std::uint32_t numFrames, const io::TimeInfo& /*t*/) noexcept override {
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            const float* src = nullptr;
            if (c < in.numChannels) src = in.channel(c);
            else if (in.numChannels > 0) src = in.channel(0);
            float* dst = out.channel(c);
            if (src) {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src[f] * gain_;
            } else {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
            }
        }
    }

private:
    float gain_;
};

// A device-free "manual pump" — the offline clock of ADR-0005. It allocates the
// planar in/out buffers ONCE (setup), then renderBlock() calls the node with
// AudioBuffer views and does NO allocation (this is exactly how a real backend
// will drive a RenderCallback in M2+).
class OfflineDriver {
public:
    OfflineDriver(std::uint32_t inChannels, std::uint32_t outChannels,
                  std::uint32_t blockSize, double sampleRate)
        : blockSize_(blockSize),
          sampleRate_(sampleRate),
          inStore_(inChannels, std::vector<float>(blockSize, 0.0f)),
          outStore_(outChannels, std::vector<float>(blockSize, 0.0f)),
          inPtrs_(inChannels),
          outPtrs_(outChannels) {
        for (std::uint32_t c = 0; c < inChannels; ++c) inPtrs_[c] = inStore_[c].data();
        for (std::uint32_t c = 0; c < outChannels; ++c) outPtrs_[c] = outStore_[c].data();
    }

    // Render one block; returns a view over the freshly-filled output buffers.
    io::AudioBuffer renderBlock(io::RenderCallback& node, std::uint64_t sampleTime) noexcept {
        io::AudioBuffer in{inPtrs_.data(),
                           static_cast<std::uint32_t>(inPtrs_.size()), blockSize_};
        io::AudioBuffer out{outPtrs_.data(),
                            static_cast<std::uint32_t>(outPtrs_.size()), blockSize_};
        const io::TimeInfo time{sampleTime,
                                static_cast<double>(sampleTime) / sampleRate_, true};
        node.process(in, out, blockSize_, time);
        return out;
    }

    // Mutable access to an input channel's buffer, to inject a known signal.
    std::vector<float>& inputChannel(std::uint32_t c) { return inStore_[c]; }

    [[nodiscard]] std::uint32_t blockSize() const noexcept { return blockSize_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

private:
    std::uint32_t blockSize_;
    double sampleRate_;
    std::vector<std::vector<float>> inStore_;
    std::vector<std::vector<float>> outStore_;
    std::vector<float*> inPtrs_;
    std::vector<float*> outPtrs_;
};

// Minimal little-endian 16-bit PCM WAV writer (enough to produce a playable file).
class WavWriter {
public:
    WavWriter(const std::string& path, std::uint32_t sampleRate, std::uint16_t channels)
        : file_(path, std::ios::binary) {
        // Header with placeholder sizes; patched in finalize().
        writeTag("RIFF");
        putU32(0);  // RIFF chunk size (patched)
        writeTag("WAVE");
        writeTag("fmt ");
        putU32(16);                              // fmt chunk size
        putU16(1);                               // PCM
        putU16(channels);
        putU32(sampleRate);
        putU32(sampleRate * channels * 2);       // byte rate
        putU16(static_cast<std::uint16_t>(channels * 2));  // block align
        putU16(16);                              // bits per sample
        writeTag("data");
        putU32(0);  // data chunk size (patched)
    }

    void writeInterleavedInt16(const std::int16_t* samples, std::size_t count) {
        file_.write(reinterpret_cast<const char*>(samples),
                    static_cast<std::streamsize>(count * sizeof(std::int16_t)));
        dataBytes_ += count * sizeof(std::int16_t);
    }

    void finalize() {
        file_.seekp(4, std::ios::beg);
        putU32(static_cast<std::uint32_t>(36 + dataBytes_));  // RIFF size
        file_.seekp(40, std::ios::beg);
        putU32(static_cast<std::uint32_t>(dataBytes_));       // data size
        file_.flush();
    }

private:
    void writeTag(const char* tag) { file_.write(tag, 4); }
    void putU32(std::uint32_t v) {
        const char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF),
                           char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)};
        file_.write(b, 4);
    }
    void putU16(std::uint16_t v) {
        const char b[2] = {char(v & 0xFF), char((v >> 8) & 0xFF)};
        file_.write(b, 2);
    }

    std::ofstream file_;
    std::size_t dataBytes_ = 0;
};

}  // namespace aiudio::examples

#include "aiudio/io/wav_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aiudio::io {
namespace {

std::uint32_t readU32(std::ifstream& f) {
    unsigned char b[4] = {0, 0, 0, 0};
    f.read(reinterpret_cast<char*>(b), 4);
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

std::uint16_t readU16(std::ifstream& f) {
    unsigned char b[2] = {0, 0};
    f.read(reinterpret_cast<char*>(b), 2);
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[0]) |
                                      (static_cast<std::uint16_t>(b[1]) << 8));
}

bool tagEquals(const char* a, const char* b) { return std::memcmp(a, b, 4) == 0; }

}  // namespace

// ---- WavReader ----------------------------------------------------------------

WavReader::WavReader(const std::string& path) : file_(path, std::ios::binary) {
    if (!file_) return;
    char tag[4];
    file_.read(tag, 4);
    if (!tagEquals(tag, "RIFF")) return;
    (void)readU32(file_);  // riff size
    file_.read(tag, 4);
    if (!tagEquals(tag, "WAVE")) return;

    bool haveFmt = false;
    std::uint32_t dataSize = 0;
    while (file_) {
        file_.read(tag, 4);
        if (file_.gcount() != 4) break;
        const std::uint32_t chunkSize = readU32(file_);
        if (tagEquals(tag, "fmt ")) {
            formatTag_ = readU16(file_);
            channels_ = readU16(file_);
            sampleRate_ = static_cast<double>(readU32(file_));
            (void)readU32(file_);  // byte rate
            (void)readU16(file_);  // block align
            bits_ = readU16(file_);
            haveFmt = true;
            // Skip any extra fmt bytes.
            if (chunkSize > 16) file_.seekg(chunkSize - 16, std::ios::cur);
        } else if (tagEquals(tag, "data")) {
            dataSize = chunkSize;
            break;  // leave the read head at the start of the data
        } else {
            file_.seekg(chunkSize + (chunkSize & 1), std::ios::cur);  // chunks are word-aligned
        }
    }

    const bool supported = haveFmt && channels_ > 0 &&
                           ((formatTag_ == 1 && bits_ == 16) || (formatTag_ == 3 && bits_ == 32));
    if (!supported || dataSize == 0) return;
    const std::uint32_t bytesPerFrame = channels_ * (bits_ / 8);
    totalFrames_ = dataSize / bytesPerFrame;
    ok_ = true;
}

std::uint32_t WavReader::read(float* const* planar, std::uint32_t channels,
                              std::uint32_t maxFrames) {
    if (!ok_) return 0;
    const std::uint32_t ch = std::min(channels, channels_);
    std::uint32_t f = 0;
    for (; f < maxFrames && framesRead_ < totalFrames_; ++f, ++framesRead_) {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            float sample = 0.0f;
            if (formatTag_ == 1) {
                const std::int16_t s = static_cast<std::int16_t>(readU16(file_));
                sample = static_cast<float>(s) / 32768.0f;
            } else {  // IEEE float32
                const std::uint32_t bits = readU32(file_);
                std::memcpy(&sample, &bits, sizeof(float));
            }
            if (c < ch) planar[c][f] = sample;
        }
    }
    // Zero any channels the caller asked for beyond the file's channel count.
    for (std::uint32_t c = ch; c < channels; ++c)
        for (std::uint32_t i = 0; i < f; ++i) planar[c][i] = 0.0f;
    return f;
}

// ---- WavWriter ----------------------------------------------------------------

WavWriter::WavWriter(const std::string& path, std::uint32_t channels, double sampleRate,
                     WavFormat format)
    : file_(path, std::ios::binary), format_(format), channels_(channels) {
    if (!file_) return;
    const std::uint16_t bits = (format_ == WavFormat::Float32) ? 32 : 16;
    const std::uint16_t tag = (format_ == WavFormat::Float32) ? 3 : 1;
    const std::uint32_t sr = static_cast<std::uint32_t>(sampleRate);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * (bits / 8));

    file_.write("RIFF", 4);
    putU32(0);  // riff size (patched)
    file_.write("WAVE", 4);
    file_.write("fmt ", 4);
    putU32(16);
    putU16(tag);
    putU16(static_cast<std::uint16_t>(channels));
    putU32(sr);
    putU32(sr * blockAlign);  // byte rate
    putU16(blockAlign);
    putU16(bits);
    file_.write("data", 4);
    putU32(0);  // data size (patched)
}

void WavWriter::write(const float* const* planar, std::uint32_t channels, std::uint32_t frames) {
    const std::uint32_t ch = std::min(channels, channels_);
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const float v = (c < ch) ? planar[c][f] : 0.0f;
            if (format_ == WavFormat::Float32) {
                std::uint32_t bits;
                std::memcpy(&bits, &v, sizeof(float));
                putU32(bits);
                dataBytes_ += 4;
            } else {
                const float clamped = std::max(-1.0f, std::min(1.0f, v));
                const auto s = static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
                putU16(static_cast<std::uint16_t>(s));
                dataBytes_ += 2;
            }
        }
    }
}

void WavWriter::finalize() {
    if (finalized_ || !file_) return;
    file_.seekp(4, std::ios::beg);
    putU32(static_cast<std::uint32_t>(36 + dataBytes_));
    file_.seekp(40, std::ios::beg);
    putU32(static_cast<std::uint32_t>(dataBytes_));
    file_.flush();
    finalized_ = true;
}

void WavWriter::putU32(std::uint32_t v) {
    const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                       static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
    file_.write(b, 4);
}

void WavWriter::putU16(std::uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
    file_.write(b, 2);
}

}  // namespace aiudio::io

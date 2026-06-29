// aiudio-io — full-duplex Core Audio backend (macOS). Milestone M4.
//
// Runs ONE IOProc on a single shared clock (ADR-0008): the device itself when
// the chosen input and output are the same device, or a private Core Audio
// aggregate device (with drift compensation) spanning the two devices otherwise.
// Drives a RenderCallback with both `in` (captured) and `out` (to fill) on the
// same clock — the basis for live capture → process → playback.
#pragma once

#ifdef __APPLE__

#include <cstdint>
#include <vector>

#include <CoreAudio/CoreAudio.h>

#include "aiudio/io/audio_backend.hpp"

namespace aiudio::io {

class RenderCallback;

class CoreAudioDuplexBackend final : public AudioBackend {
public:
    CoreAudioDuplexBackend() = default;
    ~CoreAudioDuplexBackend() override;
    CoreAudioDuplexBackend(const CoreAudioDuplexBackend&) = delete;
    CoreAudioDuplexBackend& operator=(const CoreAudioDuplexBackend&) = delete;

    std::vector<AudioDeviceInfo> enumerate() override;
    bool open(const StreamConfig& config, RenderCallback* callback) override;
    bool start() override;
    void stop() override;
    [[nodiscard]] std::uint32_t latencyFrames() const override;

    // True if open() had to create an aggregate device (input != output device).
    [[nodiscard]] bool usesAggregateDevice() const noexcept {
        return aggregateId_ != kAudioObjectUnknown;
    }

    // Invoked by the duplex IOProc (implementation detail): both the captured
    // input list and the output list to fill. RT context: alloc-/lock-free.
    void renderFromIOProc(const void* inputBufferList, void* outputBufferList) noexcept;

private:
    AudioObjectID deviceId_ = kAudioObjectUnknown;     // device or aggregate we run on
    AudioObjectID aggregateId_ = kAudioObjectUnknown;  // set if we created an aggregate
    AudioDeviceIOProcID ioProcId_ = nullptr;
    RenderCallback* callback_ = nullptr;
    double sampleRate_ = 48000.0;
    std::uint32_t inputChannels_ = 0;
    std::uint32_t outputChannels_ = 2;
    std::uint32_t maxFrames_ = 0;
    std::uint32_t latencyFrames_ = 0;
    bool running_ = false;
    std::uint64_t sampleTime_ = 0;

    std::vector<std::vector<float>> inScratch_;
    std::vector<std::vector<float>> outScratch_;
    std::vector<float*> inPtrs_;
    std::vector<float*> outPtrs_;
};

}  // namespace aiudio::io

#endif  // __APPLE__

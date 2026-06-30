// aiudio-io — Core Audio HAL output backend (macOS). Milestone M2.
//
// The first concrete AudioBackend (ADR-0005, ADR-0007): it drives a
// RenderCallback from the output device's HAL IOProc — the real-time engine
// heartbeat. Output-only for now (input = M3, full-duplex = M4).
//
// Core Audio types are kept out of this header (members are stored as primitives
// / void*) so including it does not drag CoreAudio into the rest of the tree.
#pragma once

#ifdef __APPLE__

#include <cstdint>
#include <vector>

#include <CoreAudio/CoreAudio.h>

#include "aiudio/io/audio_backend.hpp"

namespace aiudio::io {

class RenderCallback;

class CoreAudioBackend final : public AudioBackend {
public:
    CoreAudioBackend() = default;
    ~CoreAudioBackend() override;
    CoreAudioBackend(const CoreAudioBackend&) = delete;
    CoreAudioBackend& operator=(const CoreAudioBackend&) = delete;

    std::vector<AudioDeviceInfo> enumerate() override;
    bool open(const StreamConfig& config, RenderCallback* callback) override;
    bool start() override;
    void stop() override;
    [[nodiscard]] std::uint32_t latencyFrames() const override;

    /// Whether the device IOProc is currently started (control-thread view, for a
    /// frontend to poll). Set by start()/stop(); not written by the audio thread.
    [[nodiscard]] bool running() const noexcept { return running_; }

    // Invoked by the Core Audio IOProc (implementation detail). Takes the output
    // AudioBufferList as void* to keep CoreAudio out of this header. RT context:
    // must stay allocation-/lock-free.
    void renderFromIOProc(void* outputBufferList) noexcept;

    // Invoked by the HAL device-alive property listener (off the audio thread, M9.4):
    // if the device is no longer alive, fires the disconnect handler.
    void handleDeviceAliveChanged() noexcept;

private:
    AudioObjectID deviceId_ = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProcId_ = nullptr;
    RenderCallback* callback_ = nullptr;
    double sampleRate_ = 48000.0;
    std::uint32_t outputChannels_ = 2;
    std::uint32_t maxFrames_ = 0;
    std::uint32_t latencyFrames_ = 0;
    bool running_ = false;
    bool aliveListenerOn_ = false;  // HAL device-died listener registered (M9.4)
    std::uint64_t sampleTime_ = 0;

    // Pre-allocated planar scratch for interleaved-output devices (RT-safe path).
    std::vector<std::vector<float>> planarScratch_;
    std::vector<float*> planarPtrs_;
};

}  // namespace aiudio::io

#endif  // __APPLE__

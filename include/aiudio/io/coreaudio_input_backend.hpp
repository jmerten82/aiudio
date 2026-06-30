// aiudio-io — Core Audio HAL input backend (macOS). Milestone M3.
//
// Captures from an input device via its HAL IOProc and delivers each block to a
// RenderCallback as `in` (with an empty `out`) — the input-only clock of
// ADR-0005. This is the first real *producer* of audio: a consumer on another
// thread typically receives it through a lock-free RingBuffer (see the
// ex_capture_to_ringbuffer example).
#pragma once

#ifdef __APPLE__

#include <cstdint>
#include <vector>

#include <CoreAudio/CoreAudio.h>

#include "aiudio/io/audio_backend.hpp"

namespace aiudio::io {

class RenderCallback;

class CoreAudioInputBackend final : public AudioBackend {
public:
    CoreAudioInputBackend() = default;
    ~CoreAudioInputBackend() override;
    CoreAudioInputBackend(const CoreAudioInputBackend&) = delete;
    CoreAudioInputBackend& operator=(const CoreAudioInputBackend&) = delete;

    std::vector<AudioDeviceInfo> enumerate() override;
    bool open(const StreamConfig& config, RenderCallback* callback) override;
    bool start() override;
    void stop() override;
    [[nodiscard]] std::uint32_t latencyFrames() const override;

    /// Whether the input IOProc is currently started (control-thread telemetry).
    [[nodiscard]] bool running() const noexcept { return running_; }

    // Invoked by the Core Audio input IOProc (implementation detail). Takes the
    // captured AudioBufferList as const void*. RT context: allocation-/lock-free.
    void captureFromIOProc(const void* inputBufferList) noexcept;

    // HAL device-alive listener callback (off the audio thread, M9.4).
    void handleDeviceAliveChanged() noexcept;

private:
    AudioObjectID deviceId_ = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProcId_ = nullptr;
    RenderCallback* callback_ = nullptr;
    double sampleRate_ = 48000.0;
    std::uint32_t inputChannels_ = 1;
    std::uint32_t maxFrames_ = 0;
    std::uint32_t latencyFrames_ = 0;
    bool running_ = false;
    bool aliveListenerOn_ = false;  // HAL device-died listener registered (M9.4)
    std::uint64_t sampleTime_ = 0;

    // Captured audio is copied into planar scratch (mutable, RT-safe) before being
    // handed to the callback as a planar AudioBuffer view.
    std::vector<std::vector<float>> planarScratch_;
    std::vector<float*> planarPtrs_;
};

}  // namespace aiudio::io

#endif  // __APPLE__

// aiudio-io — Core Audio process-tap backend (macOS 14.4+). Milestone M5.
//
// Captures system or per-process OUTPUT audio with NO virtual device, using the
// Core Audio process-tap API (ADR-0007, docs/70): a CATapDescription → process
// tap → private aggregate device → IOProc. The tapped audio is delivered to a
// RenderCallback as `in` (empty `out`), same contract as the input backend (M3).
//
// Requires the `NSAudioCaptureUsageDescription` Info.plist key in the running
// binary + audio-capture TCC authorization (the "purple dot"). A bare unsigned
// CLI without that key captures silence; see examples/cpp (the tap examples embed
// the plist and ad-hoc sign).
//
// CATapDescription is Objective-C, so the implementation is Objective-C++ (.mm);
// no Objective-C types leak into this header.
#pragma once

#ifdef __APPLE__

#include <cstdint>
#include <string>
#include <vector>

#include <CoreAudio/CoreAudio.h>

#include "aiudio/io/audio_backend.hpp"

namespace aiudio::io {

class RenderCallback;

// A process Core Audio knows about (a candidate tap target).
struct ProcessInfo {
    int pid = 0;
    std::string bundleId;  // may be empty
    AudioObjectID objectId = kAudioObjectUnknown;
};

class CoreAudioProcessTapBackend final : public AudioBackend {
public:
    CoreAudioProcessTapBackend() = default;
    ~CoreAudioProcessTapBackend() override;
    CoreAudioProcessTapBackend(const CoreAudioProcessTapBackend&) = delete;
    CoreAudioProcessTapBackend& operator=(const CoreAudioProcessTapBackend&) = delete;

    // Configure the capture target BEFORE open() (default = whole system).
    void tapSystemAudio() noexcept;     // global tap, exclude nothing
    void tapProcess(int pid) noexcept;  // tap a single process by PID

    // List the processes Core Audio exposes (pid + bundle id). No permission needed.
    static std::vector<ProcessInfo> listProcesses();

    std::vector<AudioDeviceInfo> enumerate() override;  // device list (shared)
    bool open(const StreamConfig& config, RenderCallback* callback) override;
    bool start() override;
    void stop() override;
    [[nodiscard]] std::uint32_t latencyFrames() const override;

    // Invoked by the tap aggregate's IOProc (implementation detail). Takes the
    // captured AudioBufferList as const void*. RT context: alloc-/lock-free.
    void captureFromIOProc(const void* inputBufferList) noexcept;

private:
    int targetPid_ = -1;  // -1 → whole system
    AudioObjectID tapId_ = kAudioObjectUnknown;
    AudioObjectID aggregateId_ = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProcId_ = nullptr;
    RenderCallback* callback_ = nullptr;
    double sampleRate_ = 48000.0;
    std::uint32_t channels_ = 2;
    std::uint32_t maxFrames_ = 0;
    std::uint32_t latencyFrames_ = 0;
    bool running_ = false;
    std::uint64_t sampleTime_ = 0;

    std::vector<std::vector<float>> planarScratch_;
    std::vector<float*> planarPtrs_;
};

}  // namespace aiudio::io

#endif  // __APPLE__

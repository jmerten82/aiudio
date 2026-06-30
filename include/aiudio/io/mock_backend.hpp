// aiudio-io — MockBackend (M9.4): a deterministic, manually-pumped AudioBackend for testing
// the live path with no hardware. tick(frames) drives one callback block with a synthetic
// captured input and captures the produced output; injectDisconnect()/injectXrun() simulate
// hot-unplug and device-side xruns so the hot-plug + xrun model can be exercised headlessly
// (and in CI). Cross-platform (no Core Audio).
#pragma once

#include <cstdint>
#include <vector>

#include "aiudio/io/audio_backend.hpp"
#include "aiudio/io/audio_buffer.hpp"

namespace aiudio::io {

class RenderCallback;

class MockBackend final : public AudioBackend {
public:
    MockBackend() = default;
    ~MockBackend() override = default;
    MockBackend(const MockBackend&) = delete;
    MockBackend& operator=(const MockBackend&) = delete;

    /// The constant value the synthetic capture produces each block (default 0).
    void setInputValue(float v) noexcept { inputValue_ = v; }
    /// First sample of the last block written to output `channel` (test introspection).
    [[nodiscard]] float capturedOutput(std::uint32_t channel) const noexcept;
    [[nodiscard]] bool running() const noexcept { return running_; }

    std::vector<AudioDeviceInfo> enumerate() override;
    bool open(const StreamConfig& config, RenderCallback* callback) override;
    bool start() override;
    void stop() override;
    [[nodiscard]] std::uint32_t latencyFrames() const noexcept override { return 0; }

    /// Drive one callback block (the "clock tick"). No-op if not running / disconnected.
    void tick(std::uint32_t frames);
    /// Simulate the device going away: mark disconnected, fire the handler, stop ticking.
    void injectDisconnect();
    /// Simulate a re-plug: clear the disconnected state so start() works again.
    void injectReconnect() noexcept;
    /// Simulate a device-side xrun of `frames` (a sample-time gap / overload).
    void injectXrun(std::uint32_t frames) noexcept { addXruns(frames); }

private:
    RenderCallback* cb_ = nullptr;
    std::uint32_t inCh_ = 0, outCh_ = 0, maxFrames_ = 0;
    double sampleRate_ = 48000.0;
    bool opened_ = false, running_ = false;
    float inputValue_ = 0.0f;
    std::uint64_t sampleTime_ = 0;
    std::vector<std::vector<float>> inStore_, outStore_;
    std::vector<float*> inPtrs_, outPtrs_;
};

}  // namespace aiudio::io

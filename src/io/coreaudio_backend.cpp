#ifdef __APPLE__

#include "aiudio/io/coreaudio_backend.hpp"

#include <algorithm>

#include <CoreAudio/CoreAudio.h>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/conversions.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"
#include "coreaudio_hal.hpp"  // shared HAL property helpers (detail::)

namespace aiudio::io {
namespace {

// The output IOProc trampoline: forwards to the instance. Unused params unnamed.
OSStatus ioProcTrampoline(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*,
                          const AudioTimeStamp*, AudioBufferList* outOutputData,
                          const AudioTimeStamp*, void* clientData) {
    static_cast<CoreAudioBackend*>(clientData)->renderFromIOProc(outOutputData);
    return noErr;
}

// HAL property listener for kAudioDevicePropertyDeviceIsAlive (device-died / hot-unplug).
// Runs on a HAL notification thread, not the audio thread (M9.4).
OSStatus aliveListenerTrampoline(AudioObjectID, UInt32, const AudioObjectPropertyAddress*,
                                 void* clientData) {
    static_cast<CoreAudioBackend*>(clientData)->handleDeviceAliveChanged();
    return noErr;
}

}  // namespace

CoreAudioBackend::~CoreAudioBackend() {
    stop();
    if (aliveListenerOn_) {
        detail::removeAliveListener(deviceId_, aliveListenerTrampoline, this);
        aliveListenerOn_ = false;
    }
    if (ioProcId_ != nullptr && deviceId_ != kAudioObjectUnknown) {
        AudioDeviceDestroyIOProcID(deviceId_, ioProcId_);
    }
}

std::vector<AudioDeviceInfo> CoreAudioBackend::enumerate() {
    return detail::enumerateDevices();
}

bool CoreAudioBackend::open(const StreamConfig& config, RenderCallback* callback) {
    if (aliveListenerOn_) {  // re-open: drop the listener on the previous device
        detail::removeAliveListener(deviceId_, aliveListenerTrampoline, this);
        aliveListenerOn_ = false;
    }
    callback_ = callback;
    deviceId_ = config.outputDeviceId.empty()
                    ? detail::defaultDevice(kAudioHardwarePropertyDefaultOutputDevice)
                    : detail::findByUID(config.outputDeviceId);
    if (deviceId_ == kAudioObjectUnknown || callback_ == nullptr) return false;

    // Negotiate: read the device's reported sample rate; request the block size.
    const Float64 sr = detail::nominalSampleRate(deviceId_, kAudioObjectPropertyScopeOutput);
    if (sr > 0) sampleRate_ = sr;
    detail::requestBufferFrameSize(deviceId_, kAudioObjectPropertyScopeOutput, config.blockSize);
    const UInt32 bufferFrames =
        detail::bufferFrameSize(deviceId_, kAudioObjectPropertyScopeOutput, config.blockSize);

    outputChannels_ = detail::channelsInScope(deviceId_, kAudioObjectPropertyScopeOutput);
    if (outputChannels_ == 0) return false;
    maxFrames_ = (bufferFrames > 0) ? bufferFrames : config.blockSize;

    latencyFrames_ =
        detail::uint32Property(deviceId_, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) +
        detail::uint32Property(deviceId_, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput) +
        maxFrames_;

    // Pre-allocate the planar scratch used when the device wants interleaved output.
    planarScratch_.assign(outputChannels_, std::vector<float>(maxFrames_, 0.0f));
    planarPtrs_.assign(outputChannels_, nullptr);

    AudioDeviceIOProcID procId = nullptr;
    if (AudioDeviceCreateIOProcID(deviceId_, ioProcTrampoline, this, &procId) != noErr ||
        procId == nullptr) {
        return false;
    }
    ioProcId_ = procId;
    sampleTime_ = 0;

    // Register the device-died listener so a hot-unplug fires the disconnect handler (M9.4).
    disconnected_.store(false, std::memory_order_release);
    if (detail::addAliveListener(deviceId_, aliveListenerTrampoline, this) == noErr)
        aliveListenerOn_ = true;
    return true;
}

void CoreAudioBackend::handleDeviceAliveChanged() noexcept {
    if (!detail::deviceIsAlive(deviceId_)) notifyDisconnect();  // off the audio thread
}

void CoreAudioBackend::renderFromIOProc(void* outputBufferList) noexcept {
    auto* out = static_cast<AudioBufferList*>(outputBufferList);
    if (callback_ == nullptr || out == nullptr || out->mNumberBuffers == 0) return;

    const bool nonInterleaved = out->mNumberBuffers > 1;
    std::uint32_t frames = 0;
    if (nonInterleaved) {
        frames = out->mBuffers[0].mDataByteSize / static_cast<UInt32>(sizeof(float));
    } else {
        const UInt32 ch = out->mBuffers[0].mNumberChannels;
        frames = (ch > 0)
                     ? out->mBuffers[0].mDataByteSize / static_cast<UInt32>(sizeof(float) * ch)
                     : 0;
    }
    if (frames == 0) return;
    if (frames > maxFrames_) frames = maxFrames_;  // defensive; scratch sized to maxFrames_

    const TimeInfo time{sampleTime_, static_cast<double>(sampleTime_) / sampleRate_, true};
    const AudioBuffer inBuf{nullptr, 0, frames};

    if (nonInterleaved) {
        // One device buffer per channel → point the view straight at them (zero copy).
        const std::uint32_t ch = std::min<std::uint32_t>(out->mNumberBuffers, outputChannels_);
        for (std::uint32_t c = 0; c < ch; ++c) {
            planarPtrs_[c] = static_cast<float*>(out->mBuffers[c].mData);
        }
        AudioBuffer outBuf{planarPtrs_.data(), ch, frames};
        callback_->process(inBuf, outBuf, frames, time);
    } else {
        // Interleaved device buffer → render into planar scratch, then interleave.
        for (std::uint32_t c = 0; c < outputChannels_; ++c) {
            planarPtrs_[c] = planarScratch_[c].data();
        }
        AudioBuffer outBuf{planarPtrs_.data(), outputChannels_, frames};
        callback_->process(inBuf, outBuf, frames, time);
        interleave(planarPtrs_.data(), static_cast<float*>(out->mBuffers[0].mData),
                   outputChannels_, frames);
    }
    sampleTime_ += frames;
}

bool CoreAudioBackend::start() {
    if (ioProcId_ == nullptr || deviceId_ == kAudioObjectUnknown) return false;
    if (running_) return true;
    if (AudioDeviceStart(deviceId_, ioProcId_) != noErr) return false;
    running_ = true;
    return true;
}

void CoreAudioBackend::stop() {
    if (running_ && deviceId_ != kAudioObjectUnknown && ioProcId_ != nullptr) {
        AudioDeviceStop(deviceId_, ioProcId_);
        running_ = false;
    }
}

std::uint32_t CoreAudioBackend::latencyFrames() const { return latencyFrames_; }

}  // namespace aiudio::io

#endif  // __APPLE__

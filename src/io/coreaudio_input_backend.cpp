#ifdef __APPLE__

#include "aiudio/io/coreaudio_input_backend.hpp"

#include <cstring>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/conversions.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"
#include "coreaudio_hal.hpp"

namespace aiudio::io {
namespace {

// Input IOProc trampoline: forwards the *input* buffer list. Unused params unnamed.
OSStatus inputIOProcTrampoline(AudioObjectID, const AudioTimeStamp*,
                               const AudioBufferList* inInputData, const AudioTimeStamp*,
                               AudioBufferList*, const AudioTimeStamp*, void* clientData) {
    static_cast<CoreAudioInputBackend*>(clientData)->captureFromIOProc(inInputData);
    return noErr;
}

// HAL device-alive listener (device-died / hot-unplug); HAL notification thread (M9.4).
OSStatus inputAliveListenerTrampoline(AudioObjectID, UInt32, const AudioObjectPropertyAddress*,
                                      void* clientData) {
    static_cast<CoreAudioInputBackend*>(clientData)->handleDeviceAliveChanged();
    return noErr;
}

}  // namespace

CoreAudioInputBackend::~CoreAudioInputBackend() {
    stop();
    if (aliveListenerOn_) {
        detail::removeAliveListener(deviceId_, inputAliveListenerTrampoline, this);
        aliveListenerOn_ = false;
    }
    if (ioProcId_ != nullptr && deviceId_ != kAudioObjectUnknown) {
        AudioDeviceDestroyIOProcID(deviceId_, ioProcId_);
    }
}

std::vector<AudioDeviceInfo> CoreAudioInputBackend::enumerate() {
    return detail::enumerateDevices();
}

bool CoreAudioInputBackend::open(const StreamConfig& config, RenderCallback* callback) {
    if (aliveListenerOn_) {  // re-open: drop the listener on the previous device
        detail::removeAliveListener(deviceId_, inputAliveListenerTrampoline, this);
        aliveListenerOn_ = false;
    }
    callback_ = callback;
    deviceId_ = config.inputDeviceId.empty()
                    ? detail::defaultDevice(kAudioHardwarePropertyDefaultInputDevice)
                    : detail::findByUID(config.inputDeviceId);
    if (deviceId_ == kAudioObjectUnknown || callback_ == nullptr) return false;

    const Float64 sr = detail::nominalSampleRate(deviceId_, kAudioObjectPropertyScopeInput);
    if (sr > 0) sampleRate_ = sr;

    detail::requestBufferFrameSize(deviceId_, kAudioObjectPropertyScopeInput, config.blockSize);
    const UInt32 bufferFrames =
        detail::bufferFrameSize(deviceId_, kAudioObjectPropertyScopeInput, config.blockSize);

    inputChannels_ = detail::channelsInScope(deviceId_, kAudioObjectPropertyScopeInput);
    if (inputChannels_ == 0) return false;
    maxFrames_ = (bufferFrames > 0) ? bufferFrames : config.blockSize;

    latencyFrames_ =
        detail::uint32Property(deviceId_, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput) +
        detail::uint32Property(deviceId_, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput) +
        maxFrames_;

    planarScratch_.assign(inputChannels_, std::vector<float>(maxFrames_, 0.0f));
    planarPtrs_.assign(inputChannels_, nullptr);
    for (std::uint32_t c = 0; c < inputChannels_; ++c) planarPtrs_[c] = planarScratch_[c].data();

    AudioDeviceIOProcID procId = nullptr;
    if (AudioDeviceCreateIOProcID(deviceId_, inputIOProcTrampoline, this, &procId) != noErr ||
        procId == nullptr) {
        return false;
    }
    ioProcId_ = procId;
    sampleTime_ = 0;

    disconnected_.store(false, std::memory_order_release);
    if (detail::addAliveListener(deviceId_, inputAliveListenerTrampoline, this) == noErr)
        aliveListenerOn_ = true;
    return true;
}

void CoreAudioInputBackend::handleDeviceAliveChanged() noexcept {
    if (!detail::deviceIsAlive(deviceId_)) notifyDisconnect();  // off the audio thread
}

void CoreAudioInputBackend::captureFromIOProc(const void* inputBufferList) noexcept {
    const auto* in = static_cast<const AudioBufferList*>(inputBufferList);
    if (callback_ == nullptr || in == nullptr || in->mNumberBuffers == 0) return;

    const bool nonInterleaved = in->mNumberBuffers > 1;
    std::uint32_t frames = 0;
    if (nonInterleaved) {
        frames = in->mBuffers[0].mDataByteSize / static_cast<UInt32>(sizeof(float));
    } else {
        const UInt32 ch = in->mBuffers[0].mNumberChannels;
        frames = (ch > 0)
                     ? in->mBuffers[0].mDataByteSize / static_cast<UInt32>(sizeof(float) * ch)
                     : 0;
    }
    if (frames == 0) return;
    if (frames > maxFrames_) frames = maxFrames_;  // defensive; scratch sized to maxFrames_

    // Copy captured audio into mutable planar scratch (the device buffers are
    // read-only and may be interleaved). Then hand a planar view to the callback.
    for (std::uint32_t c = 0; c < inputChannels_; ++c) planarPtrs_[c] = planarScratch_[c].data();
    if (nonInterleaved) {
        const std::uint32_t nb =
            (in->mNumberBuffers < inputChannels_) ? in->mNumberBuffers : inputChannels_;
        for (std::uint32_t c = 0; c < nb; ++c) {
            std::memcpy(planarScratch_[c].data(), in->mBuffers[c].mData, frames * sizeof(float));
        }
        for (std::uint32_t c = nb; c < inputChannels_; ++c) {
            std::memset(planarScratch_[c].data(), 0, frames * sizeof(float));
        }
    } else {
        deinterleave(static_cast<const float*>(in->mBuffers[0].mData), planarPtrs_.data(),
                     inputChannels_, frames);
    }

    const TimeInfo time{sampleTime_, static_cast<double>(sampleTime_) / sampleRate_, true};
    AudioBuffer inBuf{planarPtrs_.data(), inputChannels_, frames};
    AudioBuffer outBuf{nullptr, 0, frames};
    callback_->process(inBuf, outBuf, frames, time);
    sampleTime_ += frames;
}

bool CoreAudioInputBackend::start() {
    if (ioProcId_ == nullptr || deviceId_ == kAudioObjectUnknown) return false;
    if (running_) return true;
    if (AudioDeviceStart(deviceId_, ioProcId_) != noErr) return false;
    running_ = true;
    return true;
}

void CoreAudioInputBackend::stop() {
    if (running_ && deviceId_ != kAudioObjectUnknown && ioProcId_ != nullptr) {
        AudioDeviceStop(deviceId_, ioProcId_);
        running_ = false;
    }
}

std::uint32_t CoreAudioInputBackend::latencyFrames() const { return latencyFrames_; }

}  // namespace aiudio::io

#endif  // __APPLE__

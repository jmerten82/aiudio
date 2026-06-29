#ifdef __APPLE__

#include "aiudio/io/coreaudio_backend.hpp"

#include <algorithm>
#include <string>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/conversions.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"

namespace aiudio::io {
namespace {

// --- Core Audio HAL property helpers (all setup-time; not RT) ---------------

inline AudioObjectPropertyAddress prop(AudioObjectPropertySelector selector,
                                       AudioObjectPropertyScope scope) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}

std::string cfToString(CFStringRef s) {
    if (s == nullptr) return {};
    char buf[512] = {0};
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return std::string(buf);
    return {};
}

std::string stringProperty(AudioObjectID dev, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress a = prop(selector, kAudioObjectPropertyScopeGlobal);
    CFStringRef str = nullptr;
    UInt32 size = sizeof(str);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &str) != noErr) return {};
    std::string out = cfToString(str);
    if (str != nullptr) CFRelease(str);
    return out;
}

std::uint32_t channelsInScope(AudioObjectID dev, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = prop(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<unsigned char> storage(size);
    auto* abl = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, abl) != noErr) return 0;
    std::uint32_t channels = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) channels += abl->mBuffers[i].mNumberChannels;
    return channels;
}

UInt32 uint32Property(AudioObjectID dev, AudioObjectPropertySelector selector,
                      AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = prop(selector, scope);
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &value);
    return value;
}

AudioObjectID defaultDevice(AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress a = prop(selector, kAudioObjectPropertyScopeGlobal);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &dev);
    return dev;
}

std::vector<AudioObjectID> allDeviceIds() {
    AudioObjectPropertyAddress a = prop(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr)
        return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr)
        return {};
    return ids;
}

AudioObjectID findByUID(const std::string& uid) {
    if (uid.empty()) return kAudioObjectUnknown;
    for (AudioObjectID id : allDeviceIds()) {
        if (stringProperty(id, kAudioDevicePropertyDeviceUID) == uid) return id;
    }
    return kAudioObjectUnknown;
}

// The C IOProc trampoline: forwards to the instance. Unused params are unnamed.
OSStatus ioProcTrampoline(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*,
                          const AudioTimeStamp*, AudioBufferList* outOutputData,
                          const AudioTimeStamp*, void* clientData) {
    static_cast<CoreAudioBackend*>(clientData)->renderFromIOProc(outOutputData);
    return noErr;
}

}  // namespace

CoreAudioBackend::~CoreAudioBackend() {
    stop();
    if (ioProcId_ != nullptr && deviceId_ != kAudioObjectUnknown) {
        AudioDeviceDestroyIOProcID(deviceId_, ioProcId_);
    }
}

std::vector<AudioDeviceInfo> CoreAudioBackend::enumerate() {
    std::vector<AudioDeviceInfo> result;
    const AudioObjectID defOut = defaultDevice(kAudioHardwarePropertyDefaultOutputDevice);
    const AudioObjectID defIn = defaultDevice(kAudioHardwarePropertyDefaultInputDevice);
    for (AudioObjectID id : allDeviceIds()) {
        AudioDeviceInfo info;
        info.id = stringProperty(id, kAudioDevicePropertyDeviceUID);
        info.name = stringProperty(id, kAudioObjectPropertyName);
        info.inputChannels = channelsInScope(id, kAudioObjectPropertyScopeInput);
        info.outputChannels = channelsInScope(id, kAudioObjectPropertyScopeOutput);
        info.isDefaultOutput = (id == defOut);
        info.isDefaultInput = (id == defIn);
        AudioObjectPropertyAddress a =
            prop(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
        Float64 sr = 0;
        UInt32 size = sizeof(sr);
        if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &sr) == noErr && sr > 0) {
            info.sampleRates.push_back(sr);
        }
        result.push_back(std::move(info));
    }
    return result;
}

bool CoreAudioBackend::open(const StreamConfig& config, RenderCallback* callback) {
    callback_ = callback;
    deviceId_ = config.outputDeviceId.empty()
                    ? defaultDevice(kAudioHardwarePropertyDefaultOutputDevice)
                    : findByUID(config.outputDeviceId);
    if (deviceId_ == kAudioObjectUnknown || callback_ == nullptr) return false;

    // Sample rate (negotiate: read what the device reports).
    {
        AudioObjectPropertyAddress a =
            prop(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeOutput);
        Float64 sr = 0;
        UInt32 size = sizeof(sr);
        if (AudioObjectGetPropertyData(deviceId_, &a, 0, nullptr, &size, &sr) == noErr && sr > 0) {
            sampleRate_ = sr;
        }
    }

    // Best-effort: request the buffer frame size, then read back the actual value.
    {
        AudioObjectPropertyAddress a =
            prop(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeOutput);
        UInt32 requested = config.blockSize;
        AudioObjectSetPropertyData(deviceId_, &a, 0, nullptr, sizeof(requested), &requested);
    }
    UInt32 bufferFrames = config.blockSize;
    {
        AudioObjectPropertyAddress a =
            prop(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeOutput);
        UInt32 size = sizeof(bufferFrames);
        AudioObjectGetPropertyData(deviceId_, &a, 0, nullptr, &size, &bufferFrames);
    }

    outputChannels_ = channelsInScope(deviceId_, kAudioObjectPropertyScopeOutput);
    if (outputChannels_ == 0) return false;
    maxFrames_ = (bufferFrames > 0) ? bufferFrames : config.blockSize;

    latencyFrames_ =
        uint32Property(deviceId_, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) +
        uint32Property(deviceId_, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput) +
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
    return true;
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
        const std::uint32_t ch =
            std::min<std::uint32_t>(out->mNumberBuffers, outputChannels_);
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
    if (AudioDeviceStart(deviceId_, ioProcId_) != noErr) {
        return false;
    }
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

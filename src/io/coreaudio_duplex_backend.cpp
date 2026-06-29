#ifdef __APPLE__

#include "aiudio/io/coreaudio_duplex_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/conversions.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"
#include "coreaudio_hal.hpp"  // shared HAL property helpers (detail::)

namespace aiudio::io {
namespace {

// Create a PRIVATE aggregate device spanning [output (master), input] with drift
// compensation on the input sub-device (ADR-0008). Private aggregates are not
// published system-wide and are torn down when the process exits. Returns the
// aggregate's AudioObjectID, or kAudioObjectUnknown on failure.
AudioObjectID createPrivateAggregate(const std::string& inputUID, const std::string& outputUID) {
    if (inputUID.empty() || outputUID.empty()) return kAudioObjectUnknown;

    static std::atomic<int> counter{0};
    const std::string uidStr = "aiudio.aggregate." + std::to_string(counter.fetch_add(1));

    CFStringRef inUID = CFStringCreateWithCString(nullptr, inputUID.c_str(), kCFStringEncodingUTF8);
    CFStringRef outUID = CFStringCreateWithCString(nullptr, outputUID.c_str(), kCFStringEncodingUTF8);
    CFStringRef aggUID = CFStringCreateWithCString(nullptr, uidStr.c_str(), kCFStringEncodingUTF8);

    auto makeSub = [](CFStringRef uid, bool drift) -> CFDictionaryRef {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(d, CFSTR(kAudioSubDeviceUIDKey), uid);
        const int32_t dc = drift ? 1 : 0;
        CFNumberRef driftNum = CFNumberCreate(nullptr, kCFNumberSInt32Type, &dc);
        CFDictionarySetValue(d, CFSTR(kAudioSubDeviceDriftCompensationKey), driftNum);
        CFRelease(driftNum);
        return d;
    };
    CFDictionaryRef outSub = makeSub(outUID, /*drift*/ false);  // clock master
    CFDictionaryRef inSub = makeSub(inUID, /*drift*/ true);     // drift-compensated
    const void* subs[] = {outSub, inSub};
    CFArrayRef subList = CFArrayCreate(nullptr, subs, 2, &kCFTypeArrayCallBacks);

    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), CFSTR("aiudio duplex"));
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), aggUID);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subList);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), outUID);
    const int32_t one = 1;
    CFNumberRef oneNum = CFNumberCreate(nullptr, kCFNumberSInt32Type, &one);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), oneNum);

    AudioObjectID agg = kAudioObjectUnknown;
    const OSStatus st = AudioHardwareCreateAggregateDevice(desc, &agg);

    CFRelease(oneNum);
    CFRelease(desc);
    CFRelease(subList);
    CFRelease(inSub);
    CFRelease(outSub);
    CFRelease(inUID);
    CFRelease(outUID);
    CFRelease(aggUID);
    return (st == noErr) ? agg : kAudioObjectUnknown;
}

// Frames carried by an AudioBufferList (handles interleaved + non-interleaved).
std::uint32_t framesOf(const AudioBufferList* abl) {
    if (abl == nullptr || abl->mNumberBuffers == 0) return 0;
    if (abl->mNumberBuffers > 1) {
        return abl->mBuffers[0].mDataByteSize / static_cast<UInt32>(sizeof(float));
    }
    const UInt32 ch = abl->mBuffers[0].mNumberChannels;
    return (ch > 0) ? abl->mBuffers[0].mDataByteSize / static_cast<UInt32>(sizeof(float) * ch) : 0;
}

OSStatus duplexIOProcTrampoline(AudioObjectID, const AudioTimeStamp*,
                                const AudioBufferList* inInputData, const AudioTimeStamp*,
                                AudioBufferList* outOutputData, const AudioTimeStamp*,
                                void* clientData) {
    static_cast<CoreAudioDuplexBackend*>(clientData)->renderFromIOProc(inInputData, outOutputData);
    return noErr;
}

}  // namespace

CoreAudioDuplexBackend::~CoreAudioDuplexBackend() {
    stop();
    if (ioProcId_ != nullptr && deviceId_ != kAudioObjectUnknown) {
        AudioDeviceDestroyIOProcID(deviceId_, ioProcId_);
    }
    if (aggregateId_ != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(aggregateId_);
    }
}

std::vector<AudioDeviceInfo> CoreAudioDuplexBackend::enumerate() {
    return detail::enumerateDevices();
}

bool CoreAudioDuplexBackend::open(const StreamConfig& config, RenderCallback* callback) {
    callback_ = callback;
    if (callback_ == nullptr) return false;

    const AudioObjectID inDev = config.inputDeviceId.empty()
                                    ? detail::defaultDevice(kAudioHardwarePropertyDefaultInputDevice)
                                    : detail::findByUID(config.inputDeviceId);
    const AudioObjectID outDev = config.outputDeviceId.empty()
                                     ? detail::defaultDevice(kAudioHardwarePropertyDefaultOutputDevice)
                                     : detail::findByUID(config.outputDeviceId);
    if (inDev == kAudioObjectUnknown || outDev == kAudioObjectUnknown) return false;

    if (inDev == outDev) {
        deviceId_ = inDev;  // one device does both — single shared clock, no aggregate
    } else {
        const std::string inUID = detail::stringProperty(inDev, kAudioDevicePropertyDeviceUID);
        const std::string outUID = detail::stringProperty(outDev, kAudioDevicePropertyDeviceUID);
        aggregateId_ = createPrivateAggregate(inUID, outUID);
        if (aggregateId_ == kAudioObjectUnknown) return false;
        deviceId_ = aggregateId_;
    }

    const Float64 sr = detail::nominalSampleRate(deviceId_, kAudioObjectPropertyScopeOutput);
    if (sr > 0) sampleRate_ = sr;
    detail::requestBufferFrameSize(deviceId_, kAudioObjectPropertyScopeOutput, config.blockSize);
    const UInt32 bufferFrames =
        detail::bufferFrameSize(deviceId_, kAudioObjectPropertyScopeOutput, config.blockSize);

    inputChannels_ = detail::channelsInScope(deviceId_, kAudioObjectPropertyScopeInput);
    outputChannels_ = detail::channelsInScope(deviceId_, kAudioObjectPropertyScopeOutput);
    if (outputChannels_ == 0) return false;
    maxFrames_ = (bufferFrames > 0) ? bufferFrames : config.blockSize;

    latencyFrames_ =
        detail::uint32Property(deviceId_, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput) +
        detail::uint32Property(deviceId_, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) +
        detail::uint32Property(deviceId_, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput) +
        detail::uint32Property(deviceId_, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput) +
        maxFrames_;

    if (inputChannels_ > 0) {
        inScratch_.assign(inputChannels_, std::vector<float>(maxFrames_, 0.0f));
        inPtrs_.assign(inputChannels_, nullptr);
        for (std::uint32_t c = 0; c < inputChannels_; ++c) inPtrs_[c] = inScratch_[c].data();
    }
    outScratch_.assign(outputChannels_, std::vector<float>(maxFrames_, 0.0f));
    outPtrs_.assign(outputChannels_, nullptr);

    AudioDeviceIOProcID procId = nullptr;
    if (AudioDeviceCreateIOProcID(deviceId_, duplexIOProcTrampoline, this, &procId) != noErr ||
        procId == nullptr) {
        return false;
    }
    ioProcId_ = procId;
    sampleTime_ = 0;
    return true;
}

void CoreAudioDuplexBackend::renderFromIOProc(const void* inputBufferList,
                                              void* outputBufferList) noexcept {
    auto* out = static_cast<AudioBufferList*>(outputBufferList);
    const auto* in = static_cast<const AudioBufferList*>(inputBufferList);
    if (callback_ == nullptr || out == nullptr || out->mNumberBuffers == 0) return;

    std::uint32_t frames = framesOf(out);
    if (frames == 0) return;
    if (frames > maxFrames_) frames = maxFrames_;

    // --- Build the input view (copy/deinterleave into scratch; empty if no input) ---
    AudioBuffer inBuf{nullptr, 0, frames};
    if (in != nullptr && in->mNumberBuffers > 0 && inputChannels_ > 0) {
        for (std::uint32_t c = 0; c < inputChannels_; ++c) inPtrs_[c] = inScratch_[c].data();
        if (in->mNumberBuffers > 1) {  // non-interleaved: one buffer per channel
            const std::uint32_t nb =
                (in->mNumberBuffers < inputChannels_) ? in->mNumberBuffers : inputChannels_;
            for (std::uint32_t c = 0; c < nb; ++c)
                std::memcpy(inScratch_[c].data(), in->mBuffers[c].mData, frames * sizeof(float));
            for (std::uint32_t c = nb; c < inputChannels_; ++c)
                std::memset(inScratch_[c].data(), 0, frames * sizeof(float));
        } else {  // interleaved
            deinterleave(static_cast<const float*>(in->mBuffers[0].mData), inPtrs_.data(),
                         inputChannels_, frames);
        }
        inBuf = AudioBuffer{inPtrs_.data(), inputChannels_, frames};
    }

    const TimeInfo time{sampleTime_, static_cast<double>(sampleTime_) / sampleRate_, true};

    // --- Build the output view and render ---
    if (out->mNumberBuffers > 1) {  // non-interleaved: write straight into device buffers
        const std::uint32_t ch = std::min<std::uint32_t>(out->mNumberBuffers, outputChannels_);
        for (std::uint32_t c = 0; c < ch; ++c) outPtrs_[c] = static_cast<float*>(out->mBuffers[c].mData);
        AudioBuffer outBuf{outPtrs_.data(), ch, frames};
        callback_->process(inBuf, outBuf, frames, time);
    } else {  // interleaved: render into scratch, then interleave into the device buffer
        for (std::uint32_t c = 0; c < outputChannels_; ++c) outPtrs_[c] = outScratch_[c].data();
        AudioBuffer outBuf{outPtrs_.data(), outputChannels_, frames};
        callback_->process(inBuf, outBuf, frames, time);
        interleave(outPtrs_.data(), static_cast<float*>(out->mBuffers[0].mData), outputChannels_,
                   frames);
    }
    sampleTime_ += frames;
}

bool CoreAudioDuplexBackend::start() {
    if (ioProcId_ == nullptr || deviceId_ == kAudioObjectUnknown) return false;
    if (running_) return true;
    if (AudioDeviceStart(deviceId_, ioProcId_) != noErr) return false;
    running_ = true;
    return true;
}

void CoreAudioDuplexBackend::stop() {
    if (running_ && deviceId_ != kAudioObjectUnknown && ioProcId_ != nullptr) {
        AudioDeviceStop(deviceId_, ioProcId_);
        running_ = false;
    }
}

std::uint32_t CoreAudioDuplexBackend::latencyFrames() const { return latencyFrames_; }

}  // namespace aiudio::io

#endif  // __APPLE__

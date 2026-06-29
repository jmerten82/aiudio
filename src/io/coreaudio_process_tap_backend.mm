#ifdef __APPLE__

#include "aiudio/io/coreaudio_process_tap_backend.hpp"

#include <cstring>
#include <string>

#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>  // AudioHardwareCreate/DestroyProcessTap
#import <CoreAudio/CATapDescription.h>      // CATapDescription (Obj-C)
#import <Foundation/Foundation.h>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/conversions.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"
#include "coreaudio_hal.hpp"  // shared detail:: HAL helpers

namespace aiudio::io {
namespace {

std::string cfToStdString(CFStringRef s) {
    if (s == nullptr) return {};
    char buf[512] = {0};
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return std::string(buf);
    return {};
}

// PID → process AudioObjectID (kAudioHardwarePropertyTranslatePIDToProcessObject).
AudioObjectID processObjectForPID(pid_t pid) {
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyTranslatePIDToProcessObject,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectID obj = kAudioObjectUnknown;
    UInt32 size = sizeof(obj);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof(pid), &pid, &size, &obj);
    return obj;
}

OSStatus tapIOProcTrampoline(AudioObjectID, const AudioTimeStamp*,
                             const AudioBufferList* inInputData, const AudioTimeStamp*,
                             AudioBufferList*, const AudioTimeStamp*, void* clientData) {
    static_cast<CoreAudioProcessTapBackend*>(clientData)->captureFromIOProc(inInputData);
    return noErr;
}

}  // namespace

CoreAudioProcessTapBackend::~CoreAudioProcessTapBackend() {
    stop();
    if (ioProcId_ != nullptr && aggregateId_ != kAudioObjectUnknown) {
        AudioDeviceDestroyIOProcID(aggregateId_, ioProcId_);
    }
    if (aggregateId_ != kAudioObjectUnknown) {
        AudioHardwareDestroyAggregateDevice(aggregateId_);
    }
    if (tapId_ != kAudioObjectUnknown) {
        AudioHardwareDestroyProcessTap(tapId_);
    }
}

void CoreAudioProcessTapBackend::tapSystemAudio() noexcept { targetPid_ = -1; }
void CoreAudioProcessTapBackend::tapProcess(int pid) noexcept { targetPid_ = pid; }

std::vector<ProcessInfo> CoreAudioProcessTapBackend::listProcesses() {
    std::vector<ProcessInfo> result;
    AudioObjectPropertyAddress listAddr{kAudioHardwarePropertyProcessObjectList,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &listAddr, 0, nullptr, &size) !=
            noErr ||
        size == 0) {
        return result;
    }
    std::vector<AudioObjectID> objects(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &listAddr, 0, nullptr, &size,
                                   objects.data()) != noErr) {
        return result;
    }
    for (AudioObjectID obj : objects) {
        ProcessInfo info;
        info.objectId = obj;
        AudioObjectPropertyAddress pidAddr{kAudioProcessPropertyPID, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        pid_t pid = 0;
        UInt32 pidSize = sizeof(pid);
        if (AudioObjectGetPropertyData(obj, &pidAddr, 0, nullptr, &pidSize, &pid) == noErr) {
            info.pid = pid;
        }
        AudioObjectPropertyAddress bidAddr{kAudioProcessPropertyBundleID,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        CFStringRef bundle = nullptr;
        UInt32 bidSize = sizeof(bundle);
        if (AudioObjectGetPropertyData(obj, &bidAddr, 0, nullptr, &bidSize, &bundle) == noErr) {
            info.bundleId = cfToStdString(bundle);
            if (bundle != nullptr) CFRelease(bundle);
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<AudioDeviceInfo> CoreAudioProcessTapBackend::enumerate() {
    return detail::enumerateDevices();
}

bool CoreAudioProcessTapBackend::open(const StreamConfig& config, RenderCallback* callback) {
    @autoreleasepool {
        callback_ = callback;
        if (callback_ == nullptr) return false;

        // 1) Build the tap description (whole system, or a single process).
        CATapDescription* desc = nil;
        if (targetPid_ < 0) {
            desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
        } else {
            const AudioObjectID proc = processObjectForPID(static_cast<pid_t>(targetPid_));
            if (proc == kAudioObjectUnknown) return false;
            desc = [[CATapDescription alloc] initStereoMixdownOfProcesses:@[ @(proc) ]];
        }
        // Message syntax (not dot syntax): `private` is a C++ keyword.
        [desc setName:@"aiudio process tap"];
        [desc setMuteBehavior:CATapUnmuted];
        [desc setPrivate:YES];

        // 2) Create the tap and read back its UUID.
        if (AudioHardwareCreateProcessTap(desc, &tapId_) != noErr || tapId_ == kAudioObjectUnknown) {
            return false;
        }
        NSString* tapUID = [[desc UUID] UUIDString];
        if (tapUID == nil) return false;

        // 3) Attach the tap to a private aggregate device (the thing we read from).
        NSDictionary* aggDesc = @{
            @(kAudioAggregateDeviceNameKey) : @"aiudio tap aggregate",
            @(kAudioAggregateDeviceUIDKey) : [NSString stringWithFormat:@"aiudio.tap.%@", tapUID],
            @(kAudioAggregateDeviceIsPrivateKey) : @YES,
            @(kAudioAggregateDeviceTapAutoStartKey) : @NO,
            @(kAudioAggregateDeviceTapListKey) : @[ @{
                @(kAudioSubTapUIDKey) : tapUID,
                @(kAudioSubTapDriftCompensationKey) : @YES,
            } ],
        };
        if (AudioHardwareCreateAggregateDevice(static_cast<CFDictionaryRef>(aggDesc),
                                               &aggregateId_) != noErr ||
            aggregateId_ == kAudioObjectUnknown) {
            return false;
        }

        // 4) Format negotiation off the aggregate's input scope.
        const Float64 sr = detail::nominalSampleRate(aggregateId_, kAudioObjectPropertyScopeInput);
        if (sr > 0) sampleRate_ = sr;
        const UInt32 bufferFrames =
            detail::bufferFrameSize(aggregateId_, kAudioObjectPropertyScopeInput, config.blockSize);
        channels_ = detail::channelsInScope(aggregateId_, kAudioObjectPropertyScopeInput);
        if (channels_ == 0) channels_ = 2;  // tap is a stereo mixdown
        maxFrames_ = (bufferFrames > 0) ? bufferFrames : config.blockSize;
        latencyFrames_ =
            detail::uint32Property(aggregateId_, kAudioDevicePropertySafetyOffset,
                                   kAudioObjectPropertyScopeInput) +
            maxFrames_;

        planarScratch_.assign(channels_, std::vector<float>(maxFrames_, 0.0f));
        planarPtrs_.assign(channels_, nullptr);
        for (std::uint32_t c = 0; c < channels_; ++c) planarPtrs_[c] = planarScratch_[c].data();

        // 5) IOProc on the aggregate — this is where tapped audio arrives.
        AudioDeviceIOProcID procId = nullptr;
        if (AudioDeviceCreateIOProcID(aggregateId_, tapIOProcTrampoline, this, &procId) != noErr ||
            procId == nullptr) {
            return false;
        }
        ioProcId_ = procId;
        sampleTime_ = 0;
        return true;
    }
}

void CoreAudioProcessTapBackend::captureFromIOProc(const void* inputBufferList) noexcept {
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
    if (frames > maxFrames_) frames = maxFrames_;

    for (std::uint32_t c = 0; c < channels_; ++c) planarPtrs_[c] = planarScratch_[c].data();
    if (nonInterleaved) {
        const std::uint32_t nb = (in->mNumberBuffers < channels_) ? in->mNumberBuffers : channels_;
        for (std::uint32_t c = 0; c < nb; ++c)
            std::memcpy(planarScratch_[c].data(), in->mBuffers[c].mData, frames * sizeof(float));
        for (std::uint32_t c = nb; c < channels_; ++c)
            std::memset(planarScratch_[c].data(), 0, frames * sizeof(float));
    } else {
        deinterleave(static_cast<const float*>(in->mBuffers[0].mData), planarPtrs_.data(), channels_,
                     frames);
    }

    const TimeInfo time{sampleTime_, static_cast<double>(sampleTime_) / sampleRate_, true};
    AudioBuffer inBuf{planarPtrs_.data(), channels_, frames};
    AudioBuffer outBuf{nullptr, 0, frames};
    callback_->process(inBuf, outBuf, frames, time);
    sampleTime_ += frames;
}

bool CoreAudioProcessTapBackend::start() {
    if (ioProcId_ == nullptr || aggregateId_ == kAudioObjectUnknown) return false;
    if (running_) return true;
    if (AudioDeviceStart(aggregateId_, ioProcId_) != noErr) return false;
    running_ = true;
    return true;
}

void CoreAudioProcessTapBackend::stop() {
    if (running_ && aggregateId_ != kAudioObjectUnknown && ioProcId_ != nullptr) {
        AudioDeviceStop(aggregateId_, ioProcId_);
        running_ = false;
    }
}

std::uint32_t CoreAudioProcessTapBackend::latencyFrames() const { return latencyFrames_; }

}  // namespace aiudio::io

#endif  // __APPLE__

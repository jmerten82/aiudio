// aiudio-io — internal Core Audio HAL helpers shared by the output (M2) and
// input (M3) backends. NOT a public header: it lives under src/ and pulls in
// CoreAudio, so only the backend .cpp files include it.
#pragma once

#ifdef __APPLE__

#include <cstdint>
#include <string>
#include <vector>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include "aiudio/io/types.hpp"

namespace aiudio::io::detail {

inline AudioObjectPropertyAddress prop(AudioObjectPropertySelector selector,
                                       AudioObjectPropertyScope scope) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}

inline std::string cfToString(CFStringRef s) {
    if (s == nullptr) return {};
    char buf[512] = {0};
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return std::string(buf);
    return {};
}

inline std::string stringProperty(AudioObjectID dev, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress a = prop(selector, kAudioObjectPropertyScopeGlobal);
    CFStringRef str = nullptr;
    UInt32 size = sizeof(str);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &str) != noErr) return {};
    std::string out = cfToString(str);
    if (str != nullptr) CFRelease(str);
    return out;
}

inline std::uint32_t channelsInScope(AudioObjectID dev, AudioObjectPropertyScope scope) {
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

inline UInt32 uint32Property(AudioObjectID dev, AudioObjectPropertySelector selector,
                             AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = prop(selector, scope);
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &value);
    return value;
}

inline Float64 nominalSampleRate(AudioObjectID dev, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = prop(kAudioDevicePropertyNominalSampleRate, scope);
    Float64 sr = 0;
    UInt32 size = sizeof(sr);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &sr) == noErr) return sr;
    return 0;
}

inline UInt32 bufferFrameSize(AudioObjectID dev, AudioObjectPropertyScope scope, UInt32 fallback) {
    AudioObjectPropertyAddress a = prop(kAudioDevicePropertyBufferFrameSize, scope);
    UInt32 frames = fallback;
    UInt32 size = sizeof(frames);
    AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &frames);
    return frames;
}

inline void requestBufferFrameSize(AudioObjectID dev, AudioObjectPropertyScope scope, UInt32 frames) {
    AudioObjectPropertyAddress a = prop(kAudioDevicePropertyBufferFrameSize, scope);
    AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof(frames), &frames);
}

inline AudioObjectID defaultDevice(AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress a = prop(selector, kAudioObjectPropertyScopeGlobal);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &dev);
    return dev;
}

inline std::vector<AudioObjectID> allDeviceIds() {
    AudioObjectPropertyAddress a = prop(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr)
        return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr)
        return {};
    return ids;
}

inline AudioObjectID findByUID(const std::string& uid) {
    if (uid.empty()) return kAudioObjectUnknown;
    for (AudioObjectID id : allDeviceIds()) {
        if (stringProperty(id, kAudioDevicePropertyDeviceUID) == uid) return id;
    }
    return kAudioObjectUnknown;
}

// ---- Device-died / hot-plug listener (M9.4 hardware wiring, M9.6) ----
// A device's `kAudioDevicePropertyDeviceIsAlive` flips to 0 when it is unplugged / dies.
// Backends register a listener on it at open() and remove it at close, so a physical
// disconnect fires AudioBackend::notifyDisconnect() (off the audio thread, on a HAL
// notification thread). The trigger itself is hardware-verified; the wiring is here.

inline bool deviceIsAlive(AudioObjectID dev) {
    if (dev == kAudioObjectUnknown) return false;
    AudioObjectPropertyAddress a = prop(kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal);
    UInt32 alive = 0;
    UInt32 size = sizeof(alive);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &alive) != noErr) return false;
    return alive != 0;
}

inline OSStatus addAliveListener(AudioObjectID dev, AudioObjectPropertyListenerProc proc,
                                 void* client) {
    if (dev == kAudioObjectUnknown) return kAudioHardwareBadObjectError;
    AudioObjectPropertyAddress a = prop(kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal);
    return AudioObjectAddPropertyListener(dev, &a, proc, client);
}

inline void removeAliveListener(AudioObjectID dev, AudioObjectPropertyListenerProc proc,
                                void* client) {
    if (dev == kAudioObjectUnknown) return;
    AudioObjectPropertyAddress a = prop(kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal);
    AudioObjectRemovePropertyListener(dev, &a, proc, client);
}

// Shared device enumeration used by both backends' enumerate().
inline std::vector<AudioDeviceInfo> enumerateDevices() {
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
        const Float64 sr = nominalSampleRate(id, kAudioObjectPropertyScopeGlobal);
        if (sr > 0) info.sampleRates.push_back(sr);
        result.push_back(std::move(info));
    }
    return result;
}

}  // namespace aiudio::io::detail

#endif  // __APPLE__

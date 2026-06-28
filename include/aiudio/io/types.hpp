// aiudio-io — shared value types for the I/O layer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aiudio::io {

/// Timing metadata passed to each render call. `sampleTime` counts frames since
/// the stream started; `hostTimeSeconds` is a monotonic wall clock.
struct TimeInfo {
    std::uint64_t sampleTime = 0;
    double hostTimeSeconds = 0.0;
    bool clockValid = false;
};

/// Describes one audio device as enumerated from a backend (e.g. Core Audio).
struct AudioDeviceInfo {
    std::string id;    ///< stable backend identifier (e.g. Core Audio UID)
    std::string name;  ///< human-readable name
    std::uint32_t inputChannels = 0;
    std::uint32_t outputChannels = 0;
    std::vector<double> sampleRates;  ///< supported sample rates, if known
    bool isDefaultInput = false;
    bool isDefaultOutput = false;
};

/// Requested stream parameters. An empty device id means "system default".
/// Either input or output channel count may be 0 (half-duplex).
struct StreamConfig {
    std::string inputDeviceId;
    std::string outputDeviceId;
    double sampleRate = 48000.0;
    std::uint32_t blockSize = 128;  ///< frames per callback
    std::uint32_t inputChannels = 0;
    std::uint32_t outputChannels = 2;
};

}  // namespace aiudio::io

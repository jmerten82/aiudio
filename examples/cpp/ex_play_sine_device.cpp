// Example: play a sine to a real output device via the Core Audio backend (M2).
//
// This is the first example that drives actual hardware through the engine
// heartbeat (CoreAudioBackend's IOProc → SineSource RenderCallback). macOS-only.
//
//   ./ex_play_sine_device --list                  # list output devices, no audio
//   ./ex_play_sine_device --device Kanto --freq 440 --seconds 3
//
// WARNING: this plays sound out of the selected device.

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef __APPLE__
#include <chrono>
#include <thread>

#include "aiudio/io/coreaudio_backend.hpp"
#include "example_support.hpp"

using namespace aiudio;

int main(int argc, char** argv) {
    bool list = false;
    std::string deviceQuery;
    double freq = 440.0;
    double seconds = 3.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") list = true;
        else if (a == "--device" && i + 1 < argc) deviceQuery = argv[++i];
        else if (a == "--freq" && i + 1 < argc) freq = std::atof(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    }

    io::CoreAudioBackend backend;
    const auto devices = backend.enumerate();

    if (list) {
        for (const auto& d : devices) {
            if (d.outputChannels > 0) {
                std::printf("%-30s out=%u%s\n", d.name.c_str(), d.outputChannels,
                            d.isDefaultOutput ? "  [default]" : "");
            }
        }
        return 0;
    }

    io::StreamConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.blockSize = 128;
    cfg.outputChannels = 2;
    if (!deviceQuery.empty()) {
        bool found = false;
        for (const auto& d : devices) {
            if (d.outputChannels > 0 && d.name.find(deviceQuery) != std::string::npos) {
                cfg.outputDeviceId = d.id;
                std::printf("using output device: %s\n", d.name.c_str());
                found = true;
                break;
            }
        }
        if (!found) {
            std::printf("no output device matching \"%s\" (try --list)\n", deviceQuery.c_str());
            return 1;
        }
    }

    examples::SineSource sine(freq, cfg.sampleRate, 0.2f);
    if (!backend.open(cfg, &sine)) {
        std::printf("backend.open() failed\n");
        return 1;
    }
    std::printf("playing %.0f Hz for %.1f s (reported latency ~%u frames). Ctrl-C to stop.\n",
                freq, seconds, backend.latencyFrames());
    if (!backend.start()) {
        std::printf("backend.start() failed\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(seconds * 1000)));
    backend.stop();
    std::printf("done\n");
    return 0;
}
#else
int main() {
    std::printf("The Core Audio backend is macOS-only.\n");
    return 0;
}
#endif

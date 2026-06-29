// Example / hands-on test for the full-duplex backend (M4): live monitoring.
//
// Routes the input straight to the output on a single shared clock (an aggregate
// device when input != output) — the first live "capture → process → playback"
// path. The "process" here is a gain; swap in any RenderCallback later.
//
//   ./ex_duplex_passthrough --seconds 10 --gain 1.0
//
// ⚠️ USE HEADPHONES — an open mic into open speakers will feed back. macOS asks
// for Microphone permission on first run.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef __APPLE__
#include <chrono>
#include <thread>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/coreaudio_duplex_backend.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"

using namespace aiudio;

// Copies input to output with gain. Mono input fans out to all output channels;
// with no input, outputs silence.
class PassthroughGain final : public io::RenderCallback {
public:
    explicit PassthroughGain(float gain) : gain_(gain) {}
    void process(const io::AudioBuffer& in, io::AudioBuffer& out, std::uint32_t numFrames,
                 const io::TimeInfo& /*t*/) noexcept override {
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            const float* src = nullptr;
            if (c < in.numChannels) src = in.channel(c);
            else if (in.numChannels > 0) src = in.channel(0);
            float* dst = out.channel(c);
            if (src) {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = src[f] * gain_;
            } else {
                for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;
            }
        }
    }

private:
    float gain_;
};

int main(int argc, char** argv) {
    double seconds = 10.0;
    float gain = 1.0f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--gain" && i + 1 < argc) gain = static_cast<float>(std::atof(argv[++i]));
    }

    io::CoreAudioDuplexBackend backend;
    io::StreamConfig cfg;  // default input + default output
    cfg.sampleRate = 48000.0;
    cfg.blockSize = 128;
    cfg.inputChannels = 1;
    cfg.outputChannels = 2;

    PassthroughGain passthrough(gain);
    if (!backend.open(cfg, &passthrough)) { std::printf("backend.open() failed\n"); return 1; }
    std::printf("full-duplex passthrough (gain %.2f) on a %s clock, latency ~%u frames.\n", gain,
                backend.usesAggregateDevice() ? "aggregate-device" : "single-device",
                backend.latencyFrames());
    std::printf("⚠️  USE HEADPHONES. Running %.0f s... Ctrl-C to stop.\n", seconds);
    if (!backend.start()) { std::printf("backend.start() failed\n"); return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(seconds * 1000)));
    backend.stop();
    std::printf("done\n");
    return 0;
}
#else
int main() { std::printf("The Core Audio duplex backend is macOS-only.\n"); return 0; }
#endif

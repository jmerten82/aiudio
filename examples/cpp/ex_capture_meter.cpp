// Example / hands-on test for the Core Audio input backend (M3): a live input
// level meter. Opens an input device, computes the level of each captured block
// in a RenderCallback (the input-only clock of ADR-0005), and prints a meter.
//
//   ./ex_capture_meter --device Sennheiser --seconds 10
//
// HANDS-ON: speak/make noise into the mic and watch the meter move. macOS asks
// for Microphone permission on first run; if denied you'll see a flat -inf meter.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef __APPLE__
#include <chrono>
#include <thread>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/coreaudio_input_backend.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"

using namespace aiudio;

// Computes the mean-square of each captured block (cheap, RT-safe) and stashes it
// in relaxed atomics for the UI thread to read. dBFS/sqrt is done off the audio
// thread, in main().
class MeterCallback final : public io::RenderCallback {
public:
    void process(const io::AudioBuffer& in, io::AudioBuffer& /*out*/,
                 std::uint32_t numFrames, const io::TimeInfo& /*t*/) noexcept override {
        double sumSq = 0.0;
        std::size_t n = 0;
        for (std::uint32_t c = 0; c < in.numChannels; ++c) {
            const float* s = in.channel(c);
            for (std::uint32_t f = 0; f < numFrames; ++f) { sumSq += double(s[f]) * s[f]; ++n; }
        }
        meanSquare_.store(n ? static_cast<float>(sumSq / double(n)) : 0.0f,
                          std::memory_order_relaxed);
        calls_.fetch_add(1, std::memory_order_relaxed);
        lastFrames_.store(numFrames, std::memory_order_relaxed);
        lastChannels_.store(in.numChannels, std::memory_order_relaxed);
    }
    float meanSquare() const { return meanSquare_.load(std::memory_order_relaxed); }
    std::uint64_t calls() const { return calls_.load(std::memory_order_relaxed); }
    std::uint32_t lastFrames() const { return lastFrames_.load(std::memory_order_relaxed); }
    std::uint32_t lastChannels() const { return lastChannels_.load(std::memory_order_relaxed); }

private:
    std::atomic<float> meanSquare_{0.0f};
    std::atomic<std::uint64_t> calls_{0};
    std::atomic<std::uint32_t> lastFrames_{0};
    std::atomic<std::uint32_t> lastChannels_{0};
};

static std::string meterBar(float dbfs, int width = 40) {
    float frac = (dbfs + 60.0f) / 60.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int filled = static_cast<int>(frac * width);
    std::string bar(1, '[');
    bar.append(filled, '#').append(width - filled, '-').append("] ");
    return bar;
}

int main(int argc, char** argv) {
    std::string deviceQuery;
    double seconds = 10.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--device" && i + 1 < argc) deviceQuery = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    }

    io::CoreAudioInputBackend backend;
    const auto devices = backend.enumerate();

    io::StreamConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.blockSize = 128;
    cfg.inputChannels = 1;
    if (!deviceQuery.empty()) {
        bool found = false;
        for (const auto& d : devices) {
            if (d.inputChannels > 0 && d.name.find(deviceQuery) != std::string::npos) {
                cfg.inputDeviceId = d.id;
                std::printf("input device: %s\n", d.name.c_str());
                found = true;
                break;
            }
        }
        if (!found) {
            std::printf("no input device matching \"%s\"\n", deviceQuery.c_str());
            return 1;
        }
    } else {
        for (const auto& d : devices)
            if (d.isDefaultInput) { std::printf("input device: %s (default)\n", d.name.c_str()); break; }
    }

    MeterCallback meter;
    if (!backend.open(cfg, &meter)) { std::printf("backend.open() failed\n"); return 1; }
    std::printf("metering for %.0f s (speak into the mic). Ctrl-C to stop.\n", seconds);
    if (!backend.start()) { std::printf("backend.start() failed\n"); return 1; }

    const int frames = static_cast<int>(seconds * 20);  // ~20 fps
    for (int i = 0; i < frames; ++i) {
        const float ms = meter.meanSquare();
        const float dbfs = 10.0f * std::log10(std::max(ms, 1e-12f));
        std::printf("\r%s%6.1f dBFS", meterBar(dbfs).c_str(), dbfs);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    backend.stop();
    std::printf("\nIOProc fired %llu times (%u frames/cb, %u ch).\n",
                static_cast<unsigned long long>(meter.calls()), meter.lastFrames(),
                meter.lastChannels());
    return 0;
}
#else
int main() { std::printf("The Core Audio input backend is macOS-only.\n"); return 0; }
#endif

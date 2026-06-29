// Example / test for the full-duplex backend (M4): a SILENT duplex probe.
//
// Opens the duplex backend (default input + default output, via a single shared
// clock — an aggregate device when they differ), outputs silence while measuring
// the captured input, and reports IOProc stats. Output is silent, so it makes no
// sound; it objectively verifies the duplex real-time path (both `in` and `out`
// present on one clock). macOS-only.
//
//   ./ex_duplex_probe --seconds 1.5

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef __APPLE__
#include <chrono>
#include <thread>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/coreaudio_duplex_backend.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"

using namespace aiudio;

class DuplexProbe final : public io::RenderCallback {
public:
    void process(const io::AudioBuffer& in, io::AudioBuffer& out, std::uint32_t numFrames,
                 const io::TimeInfo& /*t*/) noexcept override {
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            float* dst = out.channel(c);
            for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;  // silence out
        }
        double sumSq = 0.0;
        std::size_t n = 0;
        for (std::uint32_t c = 0; c < in.numChannels; ++c)
            for (std::uint32_t f = 0; f < numFrames; ++f) {
                const float v = in.channel(c)[f];
                sumSq += double(v) * v;
                ++n;
            }
        inMeanSquare_.store(n ? static_cast<float>(sumSq / double(n)) : 0.0f,
                            std::memory_order_relaxed);
        calls_.fetch_add(1, std::memory_order_relaxed);
        lastFrames_.store(numFrames, std::memory_order_relaxed);
        inChannels_.store(in.numChannels, std::memory_order_relaxed);
        outChannels_.store(out.numChannels, std::memory_order_relaxed);
    }
    std::uint64_t calls() const { return calls_.load(std::memory_order_relaxed); }
    std::uint32_t lastFrames() const { return lastFrames_.load(std::memory_order_relaxed); }
    std::uint32_t inChannels() const { return inChannels_.load(std::memory_order_relaxed); }
    std::uint32_t outChannels() const { return outChannels_.load(std::memory_order_relaxed); }
    float inMeanSquare() const { return inMeanSquare_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> calls_{0};
    std::atomic<std::uint32_t> lastFrames_{0};
    std::atomic<std::uint32_t> inChannels_{0};
    std::atomic<std::uint32_t> outChannels_{0};
    std::atomic<float> inMeanSquare_{0.0f};
};

int main(int argc, char** argv) {
    double seconds = 1.5;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    }

    io::CoreAudioDuplexBackend backend;
    io::StreamConfig cfg;  // empty device ids → default input + default output
    cfg.sampleRate = 48000.0;
    cfg.blockSize = 128;
    cfg.inputChannels = 1;
    cfg.outputChannels = 2;

    DuplexProbe probe;
    if (!backend.open(cfg, &probe)) { std::printf("backend.open() failed\n"); return 1; }
    std::printf("duplex open: %s clock, reported latency ~%u frames\n",
                backend.usesAggregateDevice() ? "aggregate-device" : "single-device",
                backend.latencyFrames());
    std::printf("probing for %.1f s (silent output)...\n", seconds);
    if (!backend.start()) { std::printf("backend.start() failed\n"); return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(seconds * 1000)));
    backend.stop();

    const float ms = probe.inMeanSquare();
    const float inDb = 10.0f * std::log10(std::max(ms, 1e-12f));
    std::printf("\nIOProc results:\n");
    std::printf("  callbacks fired : %llu\n", static_cast<unsigned long long>(probe.calls()));
    std::printf("  frames/callback : %u\n", probe.lastFrames());
    std::printf("  in channels     : %u\n", probe.inChannels());
    std::printf("  out channels    : %u\n", probe.outChannels());
    std::printf("  input level     : %.1f dBFS (%s)\n", inDb,
                ms > 1e-8f ? "live input present" : "silent/no input");

    const bool ok = probe.calls() > 0 && probe.outChannels() > 0;
    std::printf("\n%s  (duplex IOProc fired with input=%u/output=%u channels)\n",
                ok ? "PASS" : "CHECK", probe.inChannels(), probe.outChannels());
    return ok ? 0 : 1;
}
#else
int main() { std::printf("The Core Audio duplex backend is macOS-only.\n"); return 0; }
#endif

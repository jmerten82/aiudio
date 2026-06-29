// Example / test harness for the Core Audio output backend (M2).
//
// Opens the backend with a SILENT, instrumented RenderCallback and reports what
// the IOProc actually delivered: how many times it fired, the block size, the
// channel count, and the total frames vs. expected throughput. Because it outputs
// silence it makes NO audible sound — so it is safe to run repeatedly and is an
// objective check that the real-time path works (unlike ex_play_sine_device,
// which you judge by ear).
//
//   ./ex_device_probe                       # default output, ~1 s
//   ./ex_device_probe --device Kanto --seconds 2
//
// Exits 0 if the IOProc fired with sane geometry and expected throughput.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef __APPLE__
#include <chrono>
#include <thread>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/coreaudio_backend.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"

using namespace aiudio;

// Outputs silence; records IOProc stats with relaxed atomics — wait-free and
// RT-safe, so it is legal to update these from the audio thread.
class ProbeCallback final : public io::RenderCallback {
public:
    void process(const io::AudioBuffer& /*in*/, io::AudioBuffer& out,
                 std::uint32_t numFrames, const io::TimeInfo& time) noexcept override {
        for (std::uint32_t c = 0; c < out.numChannels; ++c) {
            float* dst = out.channel(c);
            for (std::uint32_t f = 0; f < numFrames; ++f) dst[f] = 0.0f;  // silence
        }
        calls_.fetch_add(1, std::memory_order_relaxed);
        totalFrames_.fetch_add(numFrames, std::memory_order_relaxed);
        lastFrames_.store(numFrames, std::memory_order_relaxed);
        lastChannels_.store(out.numChannels, std::memory_order_relaxed);
        lastSampleTime_.store(time.sampleTime, std::memory_order_relaxed);
    }

    std::uint64_t calls() const { return calls_.load(std::memory_order_relaxed); }
    std::uint64_t totalFrames() const { return totalFrames_.load(std::memory_order_relaxed); }
    std::uint32_t lastFrames() const { return lastFrames_.load(std::memory_order_relaxed); }
    std::uint32_t lastChannels() const { return lastChannels_.load(std::memory_order_relaxed); }
    std::uint64_t lastSampleTime() const { return lastSampleTime_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> calls_{0};
    std::atomic<std::uint64_t> totalFrames_{0};
    std::atomic<std::uint32_t> lastFrames_{0};
    std::atomic<std::uint32_t> lastChannels_{0};
    std::atomic<std::uint64_t> lastSampleTime_{0};
};

int main(int argc, char** argv) {
    std::string deviceQuery;
    double seconds = 1.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--device" && i + 1 < argc) deviceQuery = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    }

    io::CoreAudioBackend backend;
    const auto devices = backend.enumerate();

    io::StreamConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.blockSize = 128;
    cfg.outputChannels = 2;
    double deviceSampleRate = cfg.sampleRate;

    if (!deviceQuery.empty()) {
        bool found = false;
        for (const auto& d : devices) {
            if (d.outputChannels > 0 && d.name.find(deviceQuery) != std::string::npos) {
                cfg.outputDeviceId = d.id;
                if (!d.sampleRates.empty()) deviceSampleRate = d.sampleRates.front();
                std::printf("device: %s\n", d.name.c_str());
                found = true;
                break;
            }
        }
        if (!found) {
            std::printf("no output device matching \"%s\" (see ex_play_sine_device --list)\n",
                        deviceQuery.c_str());
            return 1;
        }
    } else {
        for (const auto& d : devices) {
            if (d.isDefaultOutput) {
                if (!d.sampleRates.empty()) deviceSampleRate = d.sampleRates.front();
                std::printf("device: %s (default output)\n", d.name.c_str());
                break;
            }
        }
    }

    ProbeCallback probe;
    if (!backend.open(cfg, &probe)) {
        std::printf("backend.open() failed\n");
        return 1;
    }
    std::printf("probing for %.1f s (silent)... reported latency ~%u frames\n", seconds,
                backend.latencyFrames());
    if (!backend.start()) {
        std::printf("backend.start() failed\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(seconds * 1000)));
    backend.stop();

    const std::uint64_t calls = probe.calls();
    const std::uint64_t total = probe.totalFrames();
    const double expected = deviceSampleRate * seconds;
    std::printf("\nIOProc results:\n");
    std::printf("  callbacks fired : %llu\n", static_cast<unsigned long long>(calls));
    std::printf("  frames/callback : %u\n", probe.lastFrames());
    std::printf("  channels        : %u\n", probe.lastChannels());
    std::printf("  total frames    : %llu (expected ~%.0f @ %.0f Hz)\n",
                static_cast<unsigned long long>(total), expected, deviceSampleRate);
    std::printf("  last sampleTime : %llu\n",
                static_cast<unsigned long long>(probe.lastSampleTime()));

    const bool firedOk = calls > 0;
    const bool geometryOk = probe.lastFrames() > 0 && probe.lastChannels() > 0;
    const bool throughputOk =
        total > 0 && static_cast<double>(total) > expected * 0.5 &&
        static_cast<double>(total) < expected * 1.5;  // tolerant of start/stop edges
    const bool ok = firedOk && geometryOk && throughputOk;
    std::printf("\n%s  (fired=%d, geometry=%d, throughput=%d)\n", ok ? "PASS" : "CHECK",
                firedOk, geometryOk, throughputOk);
    return ok ? 0 : 1;
}
#else
int main() {
    std::printf("The Core Audio backend is macOS-only.\n");
    return 0;
}
#endif

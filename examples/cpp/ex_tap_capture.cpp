// Example / hands-on test for the process-tap backend (M5): capture system or
// per-app OUTPUT audio with no virtual device, via the Core Audio tap API.
//
//   ./ex_tap_capture --system --seconds 5 system.wav     # whole-system audio
//   ./ex_tap_capture --pid 1234 --seconds 5 app.wav      # one app (see --list)
//   ./ex_tap_capture --list                              # list process PIDs
//
// Captures the tapped audio (delivered as `in`) on the audio thread, mono-mixes
// it into a lock-free RingBuffer, and the main thread drains it: it prints a live
// dBFS meter and (optionally) records to a WAV.
//
// HANDS-ON: macOS prompts for audio-recording permission on first run (the purple
// dot). Play music in an app, then run this. The example binary embeds
// NSAudioCaptureUsageDescription and is ad-hoc signed by CMake so the prompt
// appears (see docs/70 §6).

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <chrono>
#include <thread>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/conversions.hpp"
#include "aiudio/io/coreaudio_process_tap_backend.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/ring_buffer.hpp"
#include "aiudio/io/types.hpp"
#include "example_support.hpp"  // WavWriter

using namespace aiudio;

// PRODUCER (audio thread): mono-mix the tapped block into the ring + track level.
class TapToRing final : public io::RenderCallback {
public:
    TapToRing(io::RingBuffer<float>& ring, std::atomic<float>& meanSquare,
              std::atomic<std::uint64_t>& dropped)
        : ring_(ring), meanSquare_(meanSquare), dropped_(dropped) {}

    void process(const io::AudioBuffer& in, io::AudioBuffer& /*out*/, std::uint32_t numFrames,
                 const io::TimeInfo& /*t*/) noexcept override {
        float mono[4096];
        const std::uint32_t n = (numFrames < 4096) ? numFrames : 4096;
        double sumSq = 0.0;
        for (std::uint32_t f = 0; f < n; ++f) {
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < in.numChannels; ++c) sum += in.channel(c)[f];
            const float m = in.numChannels ? sum / static_cast<float>(in.numChannels) : 0.0f;
            mono[f] = m;
            sumSq += double(m) * m;
        }
        meanSquare_.store(n ? static_cast<float>(sumSq / n) : 0.0f, std::memory_order_relaxed);
        const std::size_t wrote = ring_.write(mono, n);
        if (wrote < n) dropped_.fetch_add(n - wrote, std::memory_order_relaxed);
    }

private:
    io::RingBuffer<float>& ring_;
    std::atomic<float>& meanSquare_;
    std::atomic<std::uint64_t>& dropped_;
};

int main(int argc, char** argv) {
    bool system = false;
    bool list = false;
    int pid = -1;
    double seconds = 5.0;
    std::string wavPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--system") system = true;
        else if (a == "--list") list = true;
        else if (a == "--pid" && i + 1 < argc) pid = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a.rfind("--", 0) != 0) wavPath = a;  // positional output path
    }

    if (list) {
        for (const auto& p : io::CoreAudioProcessTapBackend::listProcesses())
            std::printf("%-8d %s\n", p.pid, p.bundleId.empty() ? "(none)" : p.bundleId.c_str());
        return 0;
    }

    io::CoreAudioProcessTapBackend backend;
    if (pid >= 0) { backend.tapProcess(pid); std::printf("tapping pid %d\n", pid); }
    else { backend.tapSystemAudio(); std::printf("tapping whole-system audio\n"); }
    (void)system;

    const std::uint32_t sampleRate = 48000;
    io::RingBuffer<float> ring(sampleRate);
    std::atomic<float> meanSquare{0.0f};
    std::atomic<std::uint64_t> dropped{0};
    TapToRing producer(ring, meanSquare, dropped);

    io::StreamConfig cfg;
    cfg.sampleRate = sampleRate;
    cfg.blockSize = 128;
    if (!backend.open(cfg, &producer)) {
        std::printf("backend.open() failed (tap/aggregate not created — macOS 14.4+ required)\n");
        return 1;
    }
    std::printf("capturing %.1f s (latency ~%u frames). If silent, grant audio-recording "
                "permission and play some audio.\n", seconds, backend.latencyFrames());

    // Optional WAV recorder (mono).
    std::vector<float> buf(2048);
    std::vector<std::int16_t> pcm(2048);
    std::uint64_t totalFrames = 0;
    examples::WavWriter* wav = wavPath.empty() ? nullptr
                                               : new examples::WavWriter(wavPath, sampleRate, 1);
    auto drain = [&]() {
        std::size_t n;
        while ((n = ring.read(buf.data(), buf.size())) > 0) {
            if (wav) {
                io::floatToInt16(buf.data(), pcm.data(), n);
                wav->writeInterleavedInt16(pcm.data(), n);
            }
            totalFrames += n;
        }
    };

    if (!backend.start()) { std::printf("backend.start() failed\n"); return 1; }
    const int ticks = static_cast<int>(seconds * 20);
    for (int i = 0; i < ticks; ++i) {
        drain();
        const float db = 10.0f * std::log10(std::max(meanSquare.load(), 1e-12f));
        std::printf("\r  tapped level: %6.1f dBFS   ", db);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    backend.stop();
    drain();
    if (wav) { wav->finalize(); delete wav; }

    std::printf("\ncaptured %llu frames (~%.1f s); dropped %llu.%s\n",
                static_cast<unsigned long long>(totalFrames),
                static_cast<double>(totalFrames) / sampleRate,
                static_cast<unsigned long long>(dropped.load()),
                wavPath.empty() ? "" : (" wrote " + wavPath + " (afplay it)").c_str());
    return totalFrames > 0 ? 0 : 1;
}
#else
int main() { std::printf("Core Audio process taps are macOS-only.\n"); return 0; }
#endif

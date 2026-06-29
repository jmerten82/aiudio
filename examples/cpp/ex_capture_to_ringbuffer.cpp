// Example / hands-on test for M3 + the M1 ring buffer working together: capture
// mic input on the audio thread and ferry it to the main thread via a lock-free
// SPSC RingBuffer, which records it to a WAV file. This is the canonical
// "input IOProc → ring buffer → consumer" pattern (docs/71 M3, ADR-0004).
//
//   ./ex_capture_to_ringbuffer --device Sennheiser --seconds 5 capture.wav
//   afplay capture.wav
//
// HANDS-ON: speak during capture, then play the file back. macOS asks for
// Microphone permission on first run.

#include <atomic>
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
#include "aiudio/io/coreaudio_input_backend.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/ring_buffer.hpp"
#include "aiudio/io/types.hpp"
#include "example_support.hpp"  // WavWriter

using namespace aiudio;

// PRODUCER (audio thread): mono-mixdown each captured block into the ring buffer.
// Wait-free; counts dropped frames if the consumer ever falls behind (overrun).
class CaptureToRing final : public io::RenderCallback {
public:
    CaptureToRing(io::RingBuffer<float>& ring, std::atomic<std::uint64_t>& dropped)
        : ring_(ring), dropped_(dropped) {}

    void process(const io::AudioBuffer& in, io::AudioBuffer& /*out*/,
                 std::uint32_t numFrames, const io::TimeInfo& /*t*/) noexcept override {
        float mono[4096];
        const std::uint32_t n = (numFrames < 4096) ? numFrames : 4096;
        for (std::uint32_t f = 0; f < n; ++f) {
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < in.numChannels; ++c) sum += in.channel(c)[f];
            mono[f] = in.numChannels ? sum / static_cast<float>(in.numChannels) : 0.0f;
        }
        const std::size_t wrote = ring_.write(mono, n);
        if (wrote < n) dropped_.fetch_add(n - wrote, std::memory_order_relaxed);
    }

private:
    io::RingBuffer<float>& ring_;
    std::atomic<std::uint64_t>& dropped_;
};

int main(int argc, char** argv) {
    std::string deviceQuery;
    std::string outPath = "capture.wav";
    double seconds = 5.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--device" && i + 1 < argc) deviceQuery = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a.rfind("--", 0) != 0) outPath = a;  // positional output path
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
        if (!found) { std::printf("no input device matching \"%s\"\n", deviceQuery.c_str()); return 1; }
    }

    const auto sampleRate = static_cast<std::uint32_t>(cfg.sampleRate);
    io::RingBuffer<float> ring(sampleRate);  // ~1 s of headroom
    std::atomic<std::uint64_t> dropped{0};
    CaptureToRing producer(ring, dropped);

    if (!backend.open(cfg, &producer)) { std::printf("backend.open() failed\n"); return 1; }

    examples::WavWriter wav(outPath, sampleRate, /*channels*/ 1);
    std::vector<float> buf(2048);
    std::vector<std::int16_t> pcm(2048);
    std::uint64_t totalFrames = 0;

    auto drain = [&]() {
        std::size_t n;
        while ((n = ring.read(buf.data(), buf.size())) > 0) {
            io::floatToInt16(buf.data(), pcm.data(), n);
            wav.writeInterleavedInt16(pcm.data(), n);
            totalFrames += n;
        }
    };

    std::printf("capturing %.1f s to %s (latency ~%u frames)...\n", seconds, outPath.c_str(),
                backend.latencyFrames());
    if (!backend.start()) { std::printf("backend.start() failed\n"); return 1; }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(static_cast<long>(seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        drain();                                                  // CONSUMER (main thread)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    backend.stop();
    drain();  // flush whatever the producer left in the ring
    wav.finalize();

    std::printf("wrote %llu frames (~%.1f s) to %s; dropped %llu frames (ring overrun).\n",
                static_cast<unsigned long long>(totalFrames),
                static_cast<double>(totalFrames) / sampleRate, outPath.c_str(),
                static_cast<unsigned long long>(dropped.load()));
    std::printf("play it back:  afplay %s\n", outPath.c_str());
    return totalFrames > 0 ? 0 : 1;
}
#else
int main() { std::printf("The Core Audio input backend is macOS-only.\n"); return 0; }
#endif

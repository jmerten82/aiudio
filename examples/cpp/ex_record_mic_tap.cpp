// End-to-end signed recorder (M5 tap + M3 mic → graph → WAV), no playback.
//
//   ./aiudio-recorder --seconds 10 out.wav              # mic + WHOLE-system audio
//   ./aiudio-recorder --pid 1234 --seconds 10 out.wav   # mic + ONE app's audio
//   ./aiudio-recorder --mic-gain 1.0 --tap-gain 0.6 out.wav
//   ./aiudio-recorder --list                            # list tappable process PIDs
//
// Records the microphone AND system (or per-app) output audio, applies a configurable gain to
// each, sums them, and writes the mix to a WAV — for a duration. It is the C++ twin of the
// Python `LiveMultiSource` recorder, but PLAYBACK-FREE: the mic input IOProc is the engine
// clock, the tap is an off-clock source (its own clock) brought onto the mic timeline through a
// drift-compensated ResamplingSource, and the mixed output goes ONLY to the WAV. Because nothing
// is played to an output device, a whole-system tap cannot capture our own monitor output — no
// feedback loop.
//
// Threads (ADR-0004): the tap IOProc mono-mixes + pushes into the ResamplingSource ring; the mic
// IOProc pulls it, runs the graph, and pushes the mix into the WavRecorder ring; a writer thread
// drains that ring to disk. Only file I/O is off the RT threads.
//
// SIGNING: the tap needs NSAudioCaptureUsageDescription + a code signature; the mic needs
// NSMicrophoneUsageDescription. CMake packages this binary into a signed `aiudio-recorder.app`
// (stable bundle id → stable TCC grant). See docs/pipeline/70 §6.

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

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/coreaudio_input_backend.hpp"
#include "aiudio/io/coreaudio_process_tap_backend.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/resampling_source.hpp"
#include "aiudio/io/types.hpp"
#include "aiudio/io/wav_recorder.hpp"

using namespace aiudio;

namespace {
constexpr double kEngineRate = 48000.0;
constexpr std::uint32_t kBlock = 512;
constexpr std::uint32_t kMaxBlock = 2048;

// Mono-mix a planar block into a 1-channel scratch buffer (reused; single producer).
struct MonoMixer {
    std::vector<float> mono;
    std::vector<float*> ptr;
    io::AudioBuffer buf;
    explicit MonoMixer(std::uint32_t maxBlock) : mono(maxBlock, 0.0f), ptr(1) {
        ptr[0] = mono.data();
        buf = io::AudioBuffer{ptr.data(), 1, maxBlock};
    }
    const io::AudioBuffer& mix(const io::AudioBuffer& in, std::uint32_t frames) noexcept {
        const std::uint32_t n = frames < mono.size() ? frames : static_cast<std::uint32_t>(mono.size());
        for (std::uint32_t f = 0; f < n; ++f) {
            float s = 0.0f;
            for (std::uint32_t c = 0; c < in.numChannels; ++c) s += in.channel(c)[f];
            mono[f] = in.numChannels ? s / static_cast<float>(in.numChannels) : 0.0f;
        }
        return buf;
    }
};

// PRODUCER (tap IOProc thread): mono-mix the tapped block and push it into the ring.
class TapToSource final : public io::RenderCallback {
public:
    explicit TapToSource(io::ResamplingSource& src) : src_(src), mixer_(kMaxBlock) {}
    void process(const io::AudioBuffer& in, io::AudioBuffer& /*out*/, std::uint32_t frames,
                 const io::TimeInfo& /*t*/) noexcept override {
        if (in.numChannels == 0) return;
        src_.push(mixer_.mix(in, frames), frames);
    }

private:
    io::ResamplingSource& src_;
    MonoMixer mixer_;
};

// THE ENGINE (mic IOProc thread = the clock): mic `in` is stream 0, the drift-compensated tap is
// stream 1; run the graph and push the mix into the recorder. `out` (empty for an input device)
// is unused — nothing is played.
class MicTapEngine final : public io::RenderCallback {
public:
    MicTapEngine(graph::GraphExecutor& ex, io::ResamplingSource& tap, io::WavRecorder& rec)
        : ex_(ex), tap_(tap), rec_(rec), micMix_(kMaxBlock) {
        tapStore_.assign(kMaxBlock, 0.0f);
        tapPtr_ = tapStore_.data();
        tapBuf_ = io::AudioBuffer{&tapPtr_, 1, kMaxBlock};
        outStore_.assign(kMaxBlock, 0.0f);
        outPtr_ = outStore_.data();
        outBuf_ = io::AudioBuffer{&outPtr_, 1, kMaxBlock};
    }
    void process(const io::AudioBuffer& in, io::AudioBuffer& /*out*/, std::uint32_t frames,
                 const io::TimeInfo& t) noexcept override {
        if (frames > kMaxBlock) frames = kMaxBlock;
        tap_.pull(tapBuf_, frames);                       // tap → mic (engine) timeline
        io::AudioBuffer ins[2] = {micMix_.mix(in, frames), tapBuf_};  // stream 0 = mic, 1 = tap
        io::AudioBuffer outs[1] = {outBuf_};
        ex_.process(ins, 2, outs, 1, frames, t);          // gains + sum
        rec_.pushBlock(outBuf_, frames);                  // mix → recorder ring
        double sq = 0.0;
        for (std::uint32_t f = 0; f < frames; ++f) sq += double(outPtr_[f]) * outPtr_[f];
        meanSquare_.store(frames ? static_cast<float>(sq / frames) : 0.0f, std::memory_order_relaxed);
    }
    [[nodiscard]] float meanSquare() const noexcept { return meanSquare_.load(std::memory_order_relaxed); }

private:
    graph::GraphExecutor& ex_;
    io::ResamplingSource& tap_;
    io::WavRecorder& rec_;
    MonoMixer micMix_;
    std::vector<float> tapStore_, outStore_;
    float* tapPtr_ = nullptr;
    float* outPtr_ = nullptr;
    io::AudioBuffer tapBuf_, outBuf_;
    std::atomic<float> meanSquare_{0.0f};
};
}  // namespace

int main(int argc, char** argv) {
    bool list = false;
    int pid = -1;
    double seconds = 10.0;
    float micGain = 1.0f, tapGain = 0.6f;
    std::string wavPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") list = true;
        else if (a == "--pid" && i + 1 < argc) pid = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--mic-gain" && i + 1 < argc) micGain = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--tap-gain" && i + 1 < argc) tapGain = static_cast<float>(std::atof(argv[++i]));
        else if (a.rfind("--", 0) != 0) wavPath = a;
    }

    if (list) {
        for (const auto& p : io::CoreAudioProcessTapBackend::listProcesses())
            std::printf("%-8d %s\n", p.pid, p.bundleId.empty() ? "(none)" : p.bundleId.c_str());
        return 0;
    }
    if (wavPath.empty()) {
        std::printf("usage: aiudio-recorder [--seconds N] [--pid P] [--mic-gain G] "
                    "[--tap-gain G] out.wav\n       aiudio-recorder --list\n");
        return 2;
    }

    // Graph: source0 (mic) → gain → sum:0 ; source1 (tap) → gain → sum:1 ; sum → sink(0).
    graph::Graph g;
    const auto mic = g.addNode(std::make_unique<graph::SourceNode>(0));
    const auto micG = g.addNode(std::make_unique<graph::GainNode>(micGain));
    const auto tap = g.addNode(std::make_unique<graph::SourceNode>(1));
    const auto tapG = g.addNode(std::make_unique<graph::GainNode>(tapGain));
    const auto mix = g.addNode(std::make_unique<graph::SumNode>(2));
    const auto out = g.addNode(std::make_unique<graph::SinkNode>(0));
    g.connect(mic, 0, micG, 0);
    g.connect(micG, 0, mix, 0);
    g.connect(tap, 0, tapG, 0);
    g.connect(tapG, 0, mix, 1);
    g.connect(mix, 0, out, 0);
    graph::GraphExecutor exec;
    if (!exec.compile(g, /*channels*/ 1, kEngineRate, kMaxBlock)) {
        std::printf("graph compile failed\n");
        return 1;
    }

    io::ResamplingSource tapSource;  // tap clock → engine clock (drift-compensated ring)
    tapSource.prepare(/*channels*/ 1, /*nominalRatio*/ 1.0, /*ringFrames*/ 1 << 15, kMaxBlock);

    io::WavRecorder recorder;
    if (!recorder.start(wavPath, /*channels*/ 1, kEngineRate, io::WavFormat::Float32,
                        /*ringFrames*/ static_cast<std::uint32_t>(kEngineRate), kMaxBlock)) {
        std::printf("could not open %s for writing\n", wavPath.c_str());
        return 1;
    }

    TapToSource tapProducer(tapSource);
    MicTapEngine engine(exec, tapSource, recorder);

    // The system/per-app tap (its own clock) — the off-clock source.
    io::CoreAudioProcessTapBackend tapBackend;
    if (pid >= 0) { tapBackend.tapProcess(pid); std::printf("tapping pid %d\n", pid); }
    else { tapBackend.tapSystemAudio(); std::printf("tapping whole-system audio\n"); }
    io::StreamConfig tapCfg;
    tapCfg.sampleRate = kEngineRate;
    tapCfg.blockSize = kBlock;
    if (!tapBackend.open(tapCfg, &tapProducer)) {
        std::printf("tap open failed (macOS 14.4+; grant audio-recording permission)\n");
        return 1;
    }

    // The microphone (the clock) — its IOProc drives the engine.
    io::CoreAudioInputBackend micBackend;
    io::StreamConfig micCfg;
    micCfg.sampleRate = kEngineRate;
    micCfg.blockSize = kBlock;
    micCfg.inputChannels = 1;
    if (!micBackend.open(micCfg, &engine)) {
        std::printf("mic open failed (grant microphone permission)\n");
        return 1;
    }

    std::printf("recording %.1f s → %s   (mic-gain %.2f, tap-gain %.2f)\n", seconds,
                wavPath.c_str(), micGain, tapGain);
    std::printf("if silent: grant BOTH microphone and audio-recording permissions, and play "
                "some audio.\n");
    if (!tapBackend.start() || !micBackend.start()) {
        std::printf("backend start failed\n");
        return 1;
    }
    const int ticks = static_cast<int>(seconds * 20);
    for (int i = 0; i < ticks; ++i) {
        const float db = 10.0f * std::log10(std::max(engine.meanSquare(), 1e-12f));
        std::printf("\r  mix level: %6.1f dBFS   tap ratio %.4f fill %u   ", db,
                    tapSource.ratio(), tapSource.fillFrames());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    micBackend.stop();
    tapBackend.stop();
    recorder.stop();  // final drain + finalize the WAV

    std::printf("\nwrote %s: %llu frames (~%.1f s), dropped %llu, tap under/over %llu/%llu\n",
                wavPath.c_str(), static_cast<unsigned long long>(recorder.framesWritten()),
                static_cast<double>(recorder.framesWritten()) / kEngineRate,
                static_cast<unsigned long long>(recorder.droppedFrames()),
                static_cast<unsigned long long>(tapSource.underruns()),
                static_cast<unsigned long long>(tapSource.overruns()));
    return recorder.framesWritten() > 0 ? 0 : 1;
}
#else
int main() {
    std::printf("The mic+tap recorder is macOS-only (Core Audio input + process tap).\n");
    return 0;
}
#endif

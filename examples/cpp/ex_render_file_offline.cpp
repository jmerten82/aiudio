// Example: render a WAV through a graph offline (G4 + M6). Generates a test input
// (300 Hz + 5 kHz mix), runs it through Source → Biquad(lowpass) → Gain → Sink via
// the OfflineBackend (faster than real time), and writes the result. The lowpass
// audibly removes the 5 kHz tone. Cross-platform, no audio device.
//
//   ./ex_render_file_offline [--cutoff 1000] [--in in.wav] [--out out.wav]
//   afplay in.wav ; afplay out.wav   # compare (macOS)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "aiudio/graph/biquad_node.hpp"
#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/io/offline_backend.hpp"
#include "aiudio/io/wav_file.hpp"

using namespace aiudio;

namespace {
void generateTestInput(const std::string& path, double sampleRate, double seconds) {
    const auto n = static_cast<std::uint32_t>(seconds * sampleRate);
    std::vector<float> data(n);
    constexpr double kPi = 3.14159265358979323846;
    for (std::uint32_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        data[i] = 0.4f * std::sin(2.0 * kPi * 300.0 * t) +
                  0.4f * static_cast<float>(std::sin(2.0 * kPi * 5000.0 * t));
    }
    io::WavWriter w(path, 1, sampleRate, io::WavFormat::Int16);
    const float* ch[1] = {data.data()};
    w.write(ch, 1, n);
    w.finalize();
}
}  // namespace

int main(int argc, char** argv) {
    double cutoff = 1000.0;
    std::string inPath = "in.wav";
    std::string outPath = "out.wav";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--cutoff" && i + 1 < argc) cutoff = std::atof(argv[++i]);
        else if (a == "--in" && i + 1 < argc) inPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
    }

    generateTestInput(inPath, 48000.0, 1.0);
    std::printf("wrote test input %s (300 Hz + 5 kHz, 1 s)\n", inPath.c_str());

    io::OfflineBackend ob(inPath, outPath, io::WavFormat::Int16);
    if (!ob.inputOk()) { std::printf("could not read %s\n", inPath.c_str()); return 1; }

    // Source -> Biquad(lowpass) -> Gain(0.9) -> Sink.
    graph::Graph g;
    const auto src = g.addNode(std::make_unique<graph::SourceNode>());
    auto biquad = std::make_unique<graph::BiquadNode>(ob.inputChannels());
    biquad->setLowpass(cutoff, 0.707, ob.inputSampleRate());
    const auto bq = g.addNode(std::move(biquad));
    const auto gain = g.addNode(std::make_unique<graph::GainNode>(0.9f));
    const auto sink = g.addNode(std::make_unique<graph::SinkNode>());
    g.connect(src, 0, bq, 0);
    g.connect(bq, 0, gain, 0);
    g.connect(gain, 0, sink, 0);

    graph::GraphExecutor exec;
    if (!exec.compile(g, ob.inputChannels(), ob.inputSampleRate(), /*maxBlock*/ 1024)) {
        std::printf("compile failed\n");
        return 1;
    }
    io::StreamConfig cfg;
    cfg.blockSize = 512;
    if (!ob.open(cfg, &exec) || !ob.start()) { std::printf("render failed\n"); return 1; }

    std::printf("rendered %llu frames through Source -> Biquad(LP %.0f Hz) -> Gain -> Sink -> %s\n",
                static_cast<unsigned long long>(ob.framesRendered()), cutoff, outPath.c_str());
    std::printf("compare:  afplay %s  vs  afplay %s  (the 5 kHz tone is attenuated)\n",
                inPath.c_str(), outPath.c_str());
    return ob.framesRendered() > 0 ? 0 : 1;
}

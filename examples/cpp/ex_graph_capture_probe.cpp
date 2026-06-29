// Example / objective test for the G3 live slice: drive a compiled GRAPH from a
// real Core Audio backend, silently.
//
// Graph:  Source ─► Meter ─► Gain(0.0) ─► Sink
// The MeterNode measures the captured input; Gain(0) silences the output, so this
// makes NO sound while proving the whole path runs live on hardware:
//   duplex backend IOProc → GraphExecutor (a RenderCallback) → nodes → out.
//
//   ./ex_graph_capture_probe --seconds 1.5
//
// macOS-only (uses the Core Audio duplex backend).

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef __APPLE__
#include <chrono>
#include <thread>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/meter_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/io/coreaudio_duplex_backend.hpp"

using namespace aiudio;

int main(int argc, char** argv) {
    double seconds = 1.5;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    }

    // Build Source -> Meter -> Gain(0) -> Sink.
    graph::Graph g;
    const auto src = g.addNode(std::make_unique<graph::SourceNode>());
    const auto meterId = g.addNode(std::make_unique<graph::MeterNode>());
    const auto gain = g.addNode(std::make_unique<graph::GainNode>(0.0f));  // silence the output
    const auto sink = g.addNode(std::make_unique<graph::SinkNode>());
    g.connect(src, 0, meterId, 0);
    g.connect(meterId, 0, gain, 0);
    g.connect(gain, 0, sink, 0);

    graph::GraphExecutor exec;
    if (!exec.compile(g, /*channels*/ 2, 48000.0, /*maxBlock*/ 4096)) {
        std::printf("compile failed\n");
        return 1;
    }
    auto* meter = static_cast<graph::MeterNode*>(g.node(meterId));

    io::CoreAudioDuplexBackend backend;
    io::StreamConfig cfg;  // default input + default output
    cfg.sampleRate = 48000.0;
    cfg.blockSize = 128;
    if (!backend.open(cfg, &exec)) {  // the graph executor IS the RenderCallback
        std::printf("backend.open() failed\n");
        return 1;
    }
    std::printf("driving graph live (%s clock), silent output, %.1f s...\n",
                backend.usesAggregateDevice() ? "aggregate-device" : "single-device", seconds);
    if (!backend.start()) { std::printf("backend.start() failed\n"); return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(seconds * 1000)));
    backend.stop();

    const float ms = meter->meanSquare();
    const float dbfs = 10.0f * std::log10(std::max(ms, 1e-12f));
    std::printf("graph ran live: MeterNode fired %llu block(s); captured input %.1f dBFS (%s)\n",
                static_cast<unsigned long long>(meter->calls()), dbfs,
                ms > 1e-8f ? "live input present" : "silent/no input");

    const bool ok = meter->calls() > 0;
    std::printf("\n%s  (graph executed by the backend across %llu blocks)\n", ok ? "PASS" : "CHECK",
                static_cast<unsigned long long>(meter->calls()));
    return ok ? 0 : 1;
}
#else
int main() { std::printf("The Core Audio backends are macOS-only.\n"); return 0; }
#endif

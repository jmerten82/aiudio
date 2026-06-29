// Example / hands-on test for the G3 live slice ⭐ — the Phase-0 "first end-to-end":
// capture → graph → playback, live.
//
// Graph:  Source ─► Gain(g) ─► Meter ─► Sink
// driven by the Core Audio duplex backend (mic → speakers on one shared clock).
// The GraphExecutor IS a RenderCallback, so the backend drives the whole graph
// with no special glue.
//
//   ./ex_graph_live_passthrough --gain 1.0 --seconds 10
//
// ⚠️ USE HEADPHONES (open mic + open speakers = feedback). Mono mic appears on
// the left channel. macOS asks for Microphone permission on first run.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

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
    float gainValue = 1.0f;
    double seconds = 10.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--gain" && i + 1 < argc) gainValue = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
    }

    graph::Graph g;
    const auto src = g.addNode(std::make_unique<graph::SourceNode>());
    const auto gain = g.addNode(std::make_unique<graph::GainNode>(gainValue));
    const auto meterId = g.addNode(std::make_unique<graph::MeterNode>());
    const auto sink = g.addNode(std::make_unique<graph::SinkNode>());
    g.connect(src, 0, gain, 0);
    g.connect(gain, 0, meterId, 0);
    g.connect(meterId, 0, sink, 0);

    graph::GraphExecutor exec;
    if (!exec.compile(g, /*channels*/ 2, 48000.0, /*maxBlock*/ 4096)) {
        std::printf("compile failed\n");
        return 1;
    }
    auto* meter = static_cast<graph::MeterNode*>(g.node(meterId));

    io::CoreAudioDuplexBackend backend;
    io::StreamConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.blockSize = 128;
    if (!backend.open(cfg, &exec)) { std::printf("backend.open() failed\n"); return 1; }
    std::printf("LIVE: Source -> Gain(%.2f) -> Meter -> Sink on a %s clock. "
                "⚠️ headphones! %.0f s, Ctrl-C to stop.\n", gainValue,
                backend.usesAggregateDevice() ? "aggregate-device" : "single-device", seconds);
    if (!backend.start()) { std::printf("backend.start() failed\n"); return 1; }

    const int ticks = static_cast<int>(seconds * 20);
    for (int i = 0; i < ticks; ++i) {
        const float db = 10.0f * std::log10(std::max(meter->meanSquare(), 1e-12f));
        std::printf("\r  through-graph level: %6.1f dBFS   ", db);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    backend.stop();
    std::printf("\ndone (%llu blocks through the graph)\n",
                static_cast<unsigned long long>(meter->calls()));
    return 0;
}
#else
int main() { std::printf("The Core Audio backends are macOS-only.\n"); return 0; }
#endif

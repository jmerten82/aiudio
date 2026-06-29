// Example: compile a graph and run it through the GraphExecutor offline (G2).
//
// The executor IS a RenderCallback, so here we drive it by hand (an "offline
// pump"); in G3 a Core Audio backend drives the very same executor for live audio.
//
//   ./ex_run_graph_offline
//
// Graph:  Source ─┬─► Gain(0.5) ─► Sum:0 ─┐
//                 └─► Gain(0.8) ─► Sum:1 ─┴─► Sink

#include <cstdint>
#include <cstdio>
#include <memory>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"

using namespace aiudio::graph;

int main() {
    Graph g;
    const NodeId src = g.addNode(std::make_unique<SourceNode>());
    const NodeId ga = g.addNode(std::make_unique<GainNode>(0.5f));
    const NodeId gb = g.addNode(std::make_unique<GainNode>(0.8f));
    const NodeId mix = g.addNode(std::make_unique<SumNode>(2));
    const NodeId sink = g.addNode(std::make_unique<SinkNode>());
    g.connect(src, 0, ga, 0);
    g.connect(src, 0, gb, 0);
    g.connect(ga, 0, mix, 0);
    g.connect(gb, 0, mix, 1);
    g.connect(mix, 0, sink, 0);

    constexpr std::uint32_t kBlock = 16;
    GraphExecutor exec;
    if (!exec.compile(g, /*channels*/ 1, 48000.0, kBlock)) {
        std::printf("compile failed\n");
        return 1;
    }
    std::printf("compiled graph: %zu nodes, %zu edges\n", g.nodeCount(), g.edges().size());

    // Drive a few blocks of constant 1.0 through the executor (offline pump).
    float inData[kBlock], outData[kBlock];
    float* inCh[1] = {inData};
    float* outCh[1] = {outData};
    AudioBuffer in{inCh, 1, kBlock};
    AudioBuffer out{outCh, 1, kBlock};

    bool ok = true;
    std::uint64_t sampleTime = 0;
    for (int block = 0; block < 3; ++block) {
        for (std::uint32_t i = 0; i < kBlock; ++i) inData[i] = 1.0f;
        const TimeInfo t{sampleTime, static_cast<double>(sampleTime) / 48000.0, true};
        exec.process(in, out, kBlock, t);
        std::printf("block %d: in=1.000 -> out[0]=%.3f (expect 1.300)\n", block, outData[0]);
        ok = ok && (outData[0] > 1.299f && outData[0] < 1.301f);
        sampleTime += kBlock;
    }

    std::printf("\n%s\n", ok ? "all checks passed" : "FAILURES");
    return ok ? 0 : 1;
}

// Example: live graph edit via atomic schedule swap (G5). Drive a graph with an
// offline pump, then swap in a modified graph mid-stream and watch the output
// change — without stopping the executor. Cross-platform, no audio device.
//
//   ./ex_graph_live_edit
//
// (In a real app the swap happens while a Core Audio backend runs process() on the
//  audio thread; here we pump it by hand so the effect is visible deterministically.)

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"

using namespace aiudio::graph;

namespace {
std::unique_ptr<Graph> makeGainGraph(float gain) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    const NodeId gn = g->addNode(std::make_unique<GainNode>(gain));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, gn, 0);
    g->connect(gn, 0, k, 0);
    return g;
}

float pumpBlock(GraphExecutor& exec) {
    constexpr std::uint32_t N = 16;
    float in[N], out[N];
    for (std::uint32_t i = 0; i < N; ++i) { in[i] = 1.0f; out[i] = 0.0f; }
    float* ic[1] = {in};
    float* oc[1] = {out};
    AudioBuffer ib{ic, 1, N};
    AudioBuffer ob{oc, 1, N};
    exec.process(ib, ob, N, TimeInfo{});
    return out[0];
}
}  // namespace

int main() {
    std::vector<std::unique_ptr<Graph>> graphs;  // keep graphs alive while in use
    GraphExecutor exec;

    graphs.push_back(makeGainGraph(0.5f));
    if (!exec.compile(*graphs.back(), 1, 48000.0, 16)) { std::printf("compile failed\n"); return 1; }
    std::printf("initial graph: Source -> Gain(0.5) -> Sink\n");

    bool ok = true;
    for (int b = 0; b < 3; ++b) {
        const float v = pumpBlock(exec);
        std::printf("  block %d: in=1.0 -> out=%.3f\n", b, v);
        ok = ok && (v > 0.499f && v < 0.501f);
    }

    std::printf("--- live edit: swap in Gain(0.25) (pending retired before reclaim: %zu) ---\n",
                exec.pendingRetired());
    graphs.push_back(makeGainGraph(0.25f));
    exec.compile(*graphs.back(), 1, 48000.0, 16);  // atomic swap — executor keeps running

    for (int b = 0; b < 3; ++b) {
        const float v = pumpBlock(exec);
        std::printf("  block %d: in=1.0 -> out=%.3f\n", b, v);
        ok = ok && (v > 0.249f && v < 0.251f);
    }
    std::printf("pending retired after a few blocks: %zu (old schedule reclaimed off-thread)\n",
                exec.pendingRetired());

    std::printf("\n%s\n", ok ? "live edit took effect (0.5 -> 0.25), no stop/restart" : "FAILURES");
    return ok ? 0 : 1;
}

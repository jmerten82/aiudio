// Tests for latency reporting + delay compensation (G9, PDC): a node declares its
// latency, the executor reports the graph total, and parallel branches of differing
// latency are auto-aligned so they recombine in phase.
#include "aiudio/graph/graph_executor.hpp"

#include <cstdint>
#include <memory>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/latency_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
constexpr std::uint32_t kN = 32;

void runBlock(GraphExecutor& exec, const float* in, float* out, std::uint32_t frames) {
    float* ic[1] = {const_cast<float*>(in)};
    float* oc[1] = {out};
    AudioBuffer ib{ic, 1, frames};
    AudioBuffer ob{oc, 1, frames};
    exec.process(ib, ob, frames, TimeInfo{});
}

bool near(float a, float b) { return (a > b ? a - b : b - a) < 1e-6f; }
}  // namespace

// A node's latency is reported as the graph total.
AIUDIO_TEST(latency_reported) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    const NodeId lat = g->addNode(std::make_unique<LatencyNode>(64u, 1u));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, lat, 0);
    g->connect(lat, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, kN));
    CHECK(exec.latencyFrames() == 64);
}

// Parallel paths of differing latency recombine IN PHASE: an impulse through a
// LatencyNode(8) on one branch and direct on the other yields a single 2x impulse at
// frame 8 (both aligned), not two 1x impulses at 0 and 8.
AIUDIO_TEST(parallel_paths_realign_in_phase) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    const NodeId lat = g->addNode(std::make_unique<LatencyNode>(8u, 1u));
    const NodeId sum = g->addNode(std::make_unique<SumNode>(2));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, lat, 0);   // branch A: through the latency node (delay 8)
    g->connect(lat, 0, sum, 0);
    g->connect(s, 0, sum, 1);   // branch B: direct → executor compensates by 8
    g->connect(sum, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, kN));
    CHECK(exec.latencyFrames() == 8);

    float in[kN] = {0.0f};
    in[0] = 1.0f;  // an impulse at frame 0
    float out[kN];
    runBlock(exec, in, out, kN);

    CHECK(near(out[0], 0.0f));   // no early (uncompensated) impulse from branch B
    CHECK(near(out[7], 0.0f));
    CHECK(near(out[8], 2.0f));   // both branches land here, in phase
    CHECK(near(out[9], 0.0f));
}

// No latency node → zero latency, no compensation, output unchanged (back-compatible).
AIUDIO_TEST(zero_latency_unchanged) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    const NodeId gn = g->addNode(std::make_unique<GainNode>(0.5f));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, gn, 0);
    g->connect(gn, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, kN));
    CHECK(exec.latencyFrames() == 0);

    float in[kN];
    for (std::uint32_t i = 0; i < kN; ++i) in[i] = 1.0f;
    float out[kN];
    runBlock(exec, in, out, kN);
    CHECK(near(out[0], 0.5f));   // gain applied, no delay
}

AIUDIO_TEST_MAIN()

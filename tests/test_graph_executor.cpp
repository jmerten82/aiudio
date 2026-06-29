// Tests for the GraphExecutor (G2): compile + run, offline, golden-file style.
#include "aiudio/graph/graph_executor.hpp"

#include <cmath>
#include <cstdint>
#include <memory>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
constexpr std::uint32_t kN = 64;

// Drive the executor with a constant-`value` mono input; return out[0].
float runConstant(GraphExecutor& exec, float value) {
    float inData[kN], outData[kN];
    for (std::uint32_t i = 0; i < kN; ++i) { inData[i] = value; outData[i] = -999.0f; }
    float* inCh[1] = {inData};
    float* outCh[1] = {outData};
    AudioBuffer in{inCh, 1, kN};
    AudioBuffer out{outCh, 1, kN};
    exec.process(in, out, kN, TimeInfo{});
    return outData[0];
}
}  // namespace

AIUDIO_TEST(source_gain_sink_applies_gain) {
    Graph g;
    const NodeId src = g.addNode(std::make_unique<SourceNode>());
    const NodeId gain = g.addNode(std::make_unique<GainNode>(0.5f));
    const NodeId sink = g.addNode(std::make_unique<SinkNode>());
    g.connect(src, 0, gain, 0);
    g.connect(gain, 0, sink, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(g, /*channels*/ 1, 48000.0, kN));
    CHECK(std::fabs(runConstant(exec, 1.0f) - 0.5f) < 1e-6f);  // 1.0 * 0.5
    CHECK(std::fabs(runConstant(exec, 2.0f) - 1.0f) < 1e-6f);  // stateless, re-runnable
}

AIUDIO_TEST(fan_out_then_mix) {
    // src ─┬─► GainA(0.5) ─► Sum:0 ─┐
    //      └─► GainB(0.8) ─► Sum:1 ─┴─► Sink     (1.0 → 0.5 + 0.8 = 1.3)
    Graph g;
    const NodeId src = g.addNode(std::make_unique<SourceNode>());
    const NodeId ga = g.addNode(std::make_unique<GainNode>(0.5f));
    const NodeId gb = g.addNode(std::make_unique<GainNode>(0.8f));
    const NodeId mix = g.addNode(std::make_unique<SumNode>(2));
    const NodeId sink = g.addNode(std::make_unique<SinkNode>());
    g.connect(src, 0, ga, 0);
    g.connect(src, 0, gb, 0);  // fan-out from the source's single output port
    g.connect(ga, 0, mix, 0);
    g.connect(gb, 0, mix, 1);
    g.connect(mix, 0, sink, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(g, 1, 48000.0, kN));
    CHECK(std::fabs(runConstant(exec, 1.0f) - 1.3f) < 1e-6f);
}

AIUDIO_TEST(compile_rejects_invalid_graph) {
    Graph g;
    const NodeId a = g.addNode(std::make_unique<GainNode>());
    const NodeId b = g.addNode(std::make_unique<GainNode>());
    g.connect(a, 0, b, 0);
    g.connect(b, 0, a, 0);  // cycle
    GraphExecutor exec;
    CHECK(!exec.compile(g, 1, 48000.0, kN));
    CHECK(!exec.compiled());
}

AIUDIO_TEST_MAIN()

// Tests for the multi-stream executor (G10): N input streams + M output streams routed
// by each Source/Sink node's stream index, with the single in/out path as the 1-stream
// special case (back-compatible).
#include "aiudio/graph/graph_executor.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
constexpr std::uint32_t kN = 64;

// Run the multi-stream entry: one constant value per input stream, `numOut` outputs.
// Returns out[stream][0] for each output stream (1 channel each).
std::vector<float> runMulti(GraphExecutor& exec, const std::vector<float>& inVals,
                            std::uint32_t numOut) {
    const std::uint32_t numIn = static_cast<std::uint32_t>(inVals.size());
    std::vector<std::vector<float>> inStore(numIn), outStore(numOut);
    std::vector<float*> inCh(numIn), outCh(numOut);
    std::vector<AudioBuffer> ins(numIn), outs(numOut);
    for (std::uint32_t i = 0; i < numIn; ++i) {
        inStore[i].assign(kN, inVals[i]);
        inCh[i] = inStore[i].data();
        ins[i] = AudioBuffer{&inCh[i], 1, kN};
    }
    for (std::uint32_t i = 0; i < numOut; ++i) {
        outStore[i].assign(kN, -999.0f);
        outCh[i] = outStore[i].data();
        outs[i] = AudioBuffer{&outCh[i], 1, kN};
    }
    exec.process(ins.data(), numIn, outs.data(), numOut, kN, TimeInfo{});
    std::vector<float> r(numOut);
    for (std::uint32_t i = 0; i < numOut; ++i) r[i] = outStore[i][0];
    return r;
}

bool near(float a, float b) { return (a > b ? a - b : b - a) < 1e-6f; }
}  // namespace

// Two distinct input streams routed to two sources, mixed by a SumNode. Asymmetric gains
// prove input k reaches source k (not swapped).
AIUDIO_TEST(two_input_streams_route_and_mix) {
    auto g = std::make_unique<Graph>();
    const NodeId s0 = g->addNode(std::make_unique<SourceNode>(0));
    const NodeId s1 = g->addNode(std::make_unique<SourceNode>(1));
    const NodeId g0 = g->addNode(std::make_unique<GainNode>(0.5f));
    const NodeId g1 = g->addNode(std::make_unique<GainNode>(0.25f));
    const NodeId sum = g->addNode(std::make_unique<SumNode>(2));
    const NodeId k = g->addNode(std::make_unique<SinkNode>(0));
    g->connect(s0, 0, g0, 0);
    g->connect(s1, 0, g1, 0);
    g->connect(g0, 0, sum, 0);
    g->connect(g1, 0, sum, 1);
    g->connect(sum, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, kN));
    CHECK(exec.inputStreamCount() == 2);
    CHECK(exec.outputStreamCount() == 1);
    CHECK(exec.channels() == 1);

    CHECK(near(runMulti(exec, {1.0f, 0.0f}, 1)[0], 0.5f));    // stream0 -> gain 0.5
    CHECK(near(runMulti(exec, {0.0f, 1.0f}, 1)[0], 0.25f));   // stream1 -> gain 0.25
    CHECK(near(runMulti(exec, {1.0f, 1.0f}, 1)[0], 0.75f));   // mixed
}

// One source fanned out to two sinks on distinct output streams.
AIUDIO_TEST(two_output_streams_route_distinctly) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>(0));
    const NodeId g0 = g->addNode(std::make_unique<GainNode>(0.5f));
    const NodeId g1 = g->addNode(std::make_unique<GainNode>(0.25f));
    const NodeId k0 = g->addNode(std::make_unique<SinkNode>(0));
    const NodeId k1 = g->addNode(std::make_unique<SinkNode>(1));
    g->connect(s, 0, g0, 0);
    g->connect(s, 0, g1, 0);
    g->connect(g0, 0, k0, 0);
    g->connect(g1, 0, k1, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, kN));
    CHECK(exec.outputStreamCount() == 2);

    const std::vector<float> out = runMulti(exec, {1.0f}, 2);
    CHECK(near(out[0], 0.5f));
    CHECK(near(out[1], 0.25f));
}

// A source bound to a stream that isn't provided emits silence (no crash, no garbage).
AIUDIO_TEST(unprovided_input_stream_is_silent) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>(1));  // reads stream 1
    const NodeId gn = g->addNode(std::make_unique<GainNode>(1.0f));
    const NodeId k = g->addNode(std::make_unique<SinkNode>(0));
    g->connect(s, 0, gn, 0);
    g->connect(gn, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, kN));
    CHECK(exec.inputStreamCount() == 2);                  // max(streamIndex)+1

    CHECK(near(runMulti(exec, {0.7f}, 1)[0], 0.0f));      // only stream0 given -> source1 silent
    CHECK(near(runMulti(exec, {}, 1)[0], 0.0f));          // no inputs -> silent
}

// The single in/out RenderCallback path is the 1-stream special case and still works.
AIUDIO_TEST(single_stream_path_back_compatible) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());   // default stream 0
    const NodeId gn = g->addNode(std::make_unique<GainNode>(0.5f));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());     // default stream 0
    g->connect(s, 0, gn, 0);
    g->connect(gn, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, kN));

    float in[kN], out[kN];
    for (std::uint32_t i = 0; i < kN; ++i) { in[i] = 1.0f; out[i] = -1.0f; }
    float* ic[1] = {in};
    float* oc[1] = {out};
    AudioBuffer ib{ic, 1, kN};
    AudioBuffer ob{oc, 1, kN};
    exec.process(ib, ob, kN, TimeInfo{});                          // single-stream overload
    CHECK(near(out[0], 0.5f));
}

AIUDIO_TEST_MAIN()

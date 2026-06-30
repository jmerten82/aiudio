// Tests for the executor's xrun policy (M9.1): under-served blocks are counted and
// degraded to SILENCE (never stale/garbage); a clean run reports zero; control-command
// overflow is counted as dropped commands.
#include "aiudio/graph/graph_executor.hpp"

#include <cstdint>
#include <memory>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
constexpr std::uint32_t kBuf = 128;

std::unique_ptr<Graph> gainGraph(float gain, NodeId& gn) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    gn = g->addNode(std::make_unique<GainNode>(gain));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, gn, 0);
    g->connect(gn, 0, k, 0);
    return g;
}
bool near(float a, float b) { return (a > b ? a - b : b - a) < 1e-6f; }
}  // namespace

// Requesting more frames than the compiled maxBlock: render up to maxBlock, silence the
// tail, count one xrun.
AIUDIO_TEST(oversized_block_degrades_to_silence_and_counts) {
    NodeId gn{};
    auto g = gainGraph(1.0f, gn);
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, 64));   // maxBlock 64
    CHECK(exec.xrunCount() == 0);

    float in[kBuf], out[kBuf];
    for (std::uint32_t i = 0; i < kBuf; ++i) { in[i] = 1.0f; out[i] = -7.0f; }  // poison the tail
    float* ic[1] = {in};
    float* oc[1] = {out};
    AudioBuffer ib{ic, 1, kBuf};
    AudioBuffer ob{oc, 1, kBuf};
    exec.process(ib, ob, kBuf, TimeInfo{});       // ask for 128 (> 64)

    CHECK(near(out[0], 1.0f));    // rendered region
    CHECK(near(out[63], 1.0f));
    CHECK(near(out[64], 0.0f));   // tail silenced (not the -7 poison)
    CHECK(near(out[127], 0.0f));
    CHECK(exec.xrunCount() == 1);
}

// process() before compile() → silence, counted (no garbage, no crash).
AIUDIO_TEST(not_compiled_produces_silence) {
    GraphExecutor exec;
    float out[16];
    for (std::uint32_t i = 0; i < 16; ++i) out[i] = -7.0f;
    float in[16] = {0};
    float* ic[1] = {in};
    float* oc[1] = {out};
    AudioBuffer ib{ic, 1, 16};
    AudioBuffer ob{oc, 1, 16};
    exec.process(ib, ob, 16, TimeInfo{});
    CHECK(near(out[0], 0.0f));
    CHECK(near(out[15], 0.0f));
    CHECK(exec.xrunCount() == 1);
}

// A clean run (block <= maxBlock) never increments the xrun counter.
AIUDIO_TEST(clean_run_reports_zero) {
    NodeId gn{};
    auto g = gainGraph(0.5f, gn);
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, 128));
    float in[64], out[64];
    for (std::uint32_t i = 0; i < 64; ++i) in[i] = 1.0f;
    float* ic[1] = {in};
    float* oc[1] = {out};
    AudioBuffer ib{ic, 1, 64};
    AudioBuffer ob{oc, 1, 64};
    for (int i = 0; i < 100; ++i) exec.process(ib, ob, 64, TimeInfo{});
    CHECK(exec.xrunCount() == 0);
}

// Flooding the control-command queue without draining counts dropped commands.
AIUDIO_TEST(command_overflow_counts_dropped) {
    NodeId gn{};
    auto g = gainGraph(1.0f, gn);
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, 64));
    CHECK(exec.droppedCommands() == 0);
    for (int i = 0; i < 8192; ++i) exec.postParam(gn, GainNode::kGain, 0.5f);  // no process() drains
    CHECK(exec.droppedCommands() > 0);
}

AIUDIO_TEST_MAIN()

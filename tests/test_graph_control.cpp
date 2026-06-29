// Tests for the control plane (G7): lock-free parameter commands applied on the
// audio thread via GraphExecutor::postParam(). The control thread (here, the test —
// standing in for Python / the agent) enqueues; the audio thread drains and applies
// at the top of the next block, never blocking. The race test runs under TSan.
#include "aiudio/graph/graph_executor.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "aiudio/graph/biquad_node.hpp"
#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
constexpr std::uint32_t kN = 128;

struct GainRig {
    std::unique_ptr<Graph> graph = std::make_unique<Graph>();
    NodeId src{}, gain{}, sink{};
};

GainRig makeGainRig(float gain) {
    GainRig r;
    r.src = r.graph->addNode(std::make_unique<SourceNode>());
    r.gain = r.graph->addNode(std::make_unique<GainNode>(gain));
    r.sink = r.graph->addNode(std::make_unique<SinkNode>());
    r.graph->connect(r.src, 0, r.gain, 0);
    r.graph->connect(r.gain, 0, r.sink, 0);
    return r;
}

// Push one constant-valued block and return out[0].
float runConstant(GraphExecutor& exec, float value) {
    float in[kN], out[kN];
    for (std::uint32_t i = 0; i < kN; ++i) { in[i] = value; out[i] = -999.0f; }
    float* ic[1] = {in};
    float* oc[1] = {out};
    AudioBuffer ib{ic, 1, kN};
    AudioBuffer ob{oc, 1, kN};
    exec.process(ib, ob, kN, TimeInfo{});
    return out[0];
}

// Run a 6 kHz tone through the graph for `blocks` blocks; return the last block's RMS.
float toneRms(GraphExecutor& exec, double sr, int blocks) {
    float in[kN], out[kN];
    float* ic[1] = {in};
    float* oc[1] = {out};
    AudioBuffer ib{ic, 1, kN};
    AudioBuffer ob{oc, 1, kN};
    std::uint64_t n = 0;
    double sumSq = 0.0;
    for (int b = 0; b < blocks; ++b) {
        for (std::uint32_t i = 0; i < kN; ++i)
            in[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 6000.0 * static_cast<double>(n++) / sr));
        exec.process(ib, ob, kN, TimeInfo{});
        if (b == blocks - 1)
            for (std::uint32_t i = 0; i < kN; ++i) sumSq += static_cast<double>(out[i]) * out[i];
    }
    return static_cast<float>(std::sqrt(sumSq / kN));
}
}  // namespace

AIUDIO_TEST(post_param_applies_at_next_block) {
    GainRig r = makeGainRig(0.5f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*r.graph, 1, 48000.0, kN));

    CHECK(std::fabs(runConstant(exec, 1.0f) - 0.5f) < 1e-6f);   // initial gain
    CHECK(exec.postParam(r.gain, GainNode::kGain, 0.25f));      // queue an edit
    CHECK(std::fabs(runConstant(exec, 1.0f) - 0.25f) < 1e-6f);  // applied on the next block
    CHECK(exec.postParam(r.gain, GainNode::kGain, 2.0f));
    CHECK(std::fabs(runConstant(exec, 1.0f) - 2.0f) < 1e-6f);
}

AIUDIO_TEST(command_queued_before_compile_is_applied) {
    GainRig r = makeGainRig(0.5f);
    GraphExecutor exec;
    CHECK(exec.postParam(r.gain, GainNode::kGain, 0.1f));  // queued while not yet compiled
    REQUIRE(exec.compile(*r.graph, 1, 48000.0, kN));
    CHECK(std::fabs(runConstant(exec, 1.0f) - 0.1f) < 1e-6f);  // drained on first block
}

AIUDIO_TEST(unknown_node_id_is_ignored) {
    GainRig r = makeGainRig(0.5f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*r.graph, 1, 48000.0, kN));
    CHECK(exec.postParam(99999u, GainNode::kGain, 0.0f));  // out-of-range target
    CHECK(std::fabs(runConstant(exec, 1.0f) - 0.5f) < 1e-6f);  // unchanged, no crash
}

AIUDIO_TEST(biquad_cutoff_is_live_controllable) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    auto bq = std::make_unique<BiquadNode>(1);
    bq->setLowpass(500.0, 0.707, 48000.0);  // tight low-pass: heavily attenuates 6 kHz
    const NodeId lp = g->addNode(std::move(bq));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, lp, 0);
    g->connect(lp, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, kN));

    const float rmsLow = toneRms(exec, 48000.0, 24);            // cutoff 500 Hz
    CHECK(exec.postParam(lp, BiquadNode::kCutoffHz, 18000.0f));  // open the filter wide
    const float rmsHigh = toneRms(exec, 48000.0, 24);           // cutoff 18 kHz
    CHECK(rmsHigh > rmsLow * 3.0f);                              // the 6 kHz tone now passes
}

AIUDIO_TEST(queue_is_bounded_and_never_allocates) {
    GainRig r = makeGainRig(1.0f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*r.graph, 1, 48000.0, kN));
    // Flood without draining (no process()): push must eventually report "full" rather
    // than grow/allocate. (Capacity is fixed at construction.)
    bool sawFull = false;
    for (int i = 0; i < 4096; ++i)
        if (!exec.postParam(r.gain, GainNode::kGain, 0.5f)) { sawFull = true; break; }
    CHECK(sawFull);
}

// Acceptance test: flood parameter edits from a control thread while another thread
// runs process() in a tight loop. Must be race-free (run under TSan), output bounded.
AIUDIO_TEST(control_plane_is_race_free) {
    GainRig r = makeGainRig(0.5f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*r.graph, 1, 48000.0, kN));

    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};

    std::thread audio([&] {
        float in[kN], out[kN];
        for (std::uint32_t i = 0; i < kN; ++i) in[i] = 1.0f;
        float* ic[1] = {in};
        float* oc[1] = {out};
        AudioBuffer ib{ic, 1, kN};
        AudioBuffer ob{oc, 1, kN};
        while (!stop.load(std::memory_order_relaxed)) {
            exec.process(ib, ob, kN, TimeInfo{});
            const float v = out[0];
            if (!std::isfinite(v) || v < -0.0001f || v > 1.0001f)  // gain in [0,1] -> out in [0,1]
                bad.store(true, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 200000; ++i)
        exec.postParam(r.gain, GainNode::kGain, 0.0005f * static_cast<float>(i % 2000));
    stop.store(true, std::memory_order_relaxed);
    audio.join();

    CHECK(!bad.load());
    CHECK(exec.renderCount() > 0);  // telemetry: the audio thread actually ran
}

AIUDIO_TEST_MAIN()

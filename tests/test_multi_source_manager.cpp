// Tests for the MultiSourceManager (M10): N input sources + M output sinks composed onto
// one clock via per-(stream,channel) SPSC rings + the multi-stream executor (G10).
// Functional (mix, fan-out), degradation (underrun→silence, overrun telemetry), and a
// multithreaded acceptance test (run under TSan) for the real cross-thread scenario.
#include "aiudio/graph/multi_source_manager.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
bool near(float a, float b) { return (a > b ? a - b : b - a) < 1e-6f; }

// One-channel planar AudioBuffer over `buf`.
AudioBuffer mono(float* buf, std::uint32_t frames) {
    static thread_local float* p[1];
    p[0] = buf;
    return AudioBuffer{p, 1, frames};
}

void pushConst(MultiSourceManager& m, std::uint32_t stream, float value, std::uint32_t frames) {
    std::vector<float> b(frames, value);
    AudioBuffer src = mono(b.data(), frames);
    m.pushInput(stream, src, frames);
}

float popFirst(MultiSourceManager& m, std::uint32_t stream, std::uint32_t frames) {
    std::vector<float> b(frames, -999.0f);
    AudioBuffer dst = mono(b.data(), frames);
    m.popOutput(stream, dst, frames);
    return b[0];
}
}  // namespace

// Two input sources mixed by a SumNode → one output. Asymmetric gains prove input k → source k.
AIUDIO_TEST(two_inputs_mix_to_one_output) {
    auto g = std::make_unique<Graph>();
    const NodeId s0 = g->addNode(std::make_unique<SourceNode>(0));
    const NodeId s1 = g->addNode(std::make_unique<SourceNode>(1));
    const NodeId g0 = g->addNode(std::make_unique<GainNode>(1.0f));
    const NodeId g1 = g->addNode(std::make_unique<GainNode>(1.0f));
    const NodeId sum = g->addNode(std::make_unique<SumNode>(2));
    const NodeId k = g->addNode(std::make_unique<SinkNode>(0));
    g->connect(s0, 0, g0, 0);
    g->connect(s1, 0, g1, 0);
    g->connect(g0, 0, sum, 0);
    g->connect(g1, 0, sum, 1);
    g->connect(sum, 0, k, 0);
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, 64));

    MultiSourceManager mgr(/*in*/ 2, /*out*/ 1, /*ch*/ 1, /*maxBlock*/ 64, /*ringFrames*/ 256);
    CHECK(mgr.numInputs() == 2);
    CHECK(mgr.numOutputs() == 1);
    pushConst(mgr, 0, 0.5f, 32);
    pushConst(mgr, 1, 0.3f, 32);
    mgr.process(exec, 32, TimeInfo{});
    CHECK(near(popFirst(mgr, 0, 32), 0.8f));
}

// One source fanned out to two output sinks at different gains.
AIUDIO_TEST(one_input_two_outputs) {
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
    REQUIRE(exec.compile(*g, 1, 48000.0, 64));

    MultiSourceManager mgr(1, 2, 1, 64, 256);
    pushConst(mgr, 0, 1.0f, 32);
    mgr.process(exec, 32, TimeInfo{});
    CHECK(near(popFirst(mgr, 0, 32), 0.5f));
    CHECK(near(popFirst(mgr, 1, 32), 0.25f));
}

// Pumping without feeding an input → silence + counted input underrun.
AIUDIO_TEST(input_underrun_is_silent_and_counted) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>(0));
    const NodeId k = g->addNode(std::make_unique<SinkNode>(0));
    g->connect(s, 0, k, 0);
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, 64));

    MultiSourceManager mgr(1, 1, 1, 64, 256);
    mgr.process(exec, 32, TimeInfo{});       // no pushInput → input ring empty
    CHECK(near(popFirst(mgr, 0, 32), 0.0f));  // silence, not garbage
    CHECK(mgr.inputUnderruns(0) > 0);
}

// Pumping faster than the sink drains → output ring fills → counted output overrun.
AIUDIO_TEST(output_overrun_is_counted) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>(0));
    const NodeId k = g->addNode(std::make_unique<SinkNode>(0));
    g->connect(s, 0, k, 0);
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, 64));

    MultiSourceManager mgr(1, 1, 1, 64, /*ringFrames*/ 128);  // small output ring
    for (int i = 0; i < 8; ++i) {                              // feed + pump, never pop
        pushConst(mgr, 0, 1.0f, 64);
        mgr.process(exec, 64, TimeInfo{});
    }
    CHECK(mgr.outputOverruns(0) > 0);
}

// Acceptance: producer, pump, and consumer on three threads. Race-free (run under TSan),
// output bounded (each sample is silence or the gained input, never garbage).
AIUDIO_TEST(multithreaded_is_race_free) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>(0));
    const NodeId gn = g->addNode(std::make_unique<GainNode>(1.0f));
    const NodeId k = g->addNode(std::make_unique<SinkNode>(0));
    g->connect(s, 0, gn, 0);
    g->connect(gn, 0, k, 0);
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, 48000.0, 64));

    MultiSourceManager mgr(1, 1, 1, 64, 512);
    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};
    std::atomic<std::uint64_t> pumps{0};

    std::thread producer([&] {
        std::vector<float> b(64, 1.0f);
        while (!stop.load(std::memory_order_relaxed)) {
            AudioBuffer src{nullptr, 1, 64};
            float* p[1] = {b.data()};
            src.channels = p;
            mgr.pushInput(0, src, 64);
        }
    });
    std::thread pump([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            mgr.process(exec, 64, TimeInfo{});
            pumps.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread consumer([&] {
        std::vector<float> b(64, 0.0f);
        while (!stop.load(std::memory_order_relaxed)) {
            float* p[1] = {b.data()};
            AudioBuffer dst{p, 1, 64};
            mgr.popOutput(0, dst, 64);
            for (float v : b)
                if (!std::isfinite(v) || v < -0.0001f || v > 1.0001f)
                    bad.store(true, std::memory_order_relaxed);
        }
    });

    while (pumps.load(std::memory_order_relaxed) < 2000) { /* let it run */ }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    pump.join();
    consumer.join();

    CHECK(!bad.load());
    CHECK(pumps.load() > 0);
}

AIUDIO_TEST_MAIN()

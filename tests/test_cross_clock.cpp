// Tests for cross-clock multi-device (M9.6): one input device (master clock) + one output
// device on a SEPARATE clock, kept in sync via the CrossClockBridge's drift-compensated
// output path. Covers signal flow across the clock boundary (DC preserved), and soaks where
// the output device's clock runs slow/fast — the ring must stay bounded (no overrun when the
// consumer lags; no persistent underrun when it races). A multithreaded run (A's IOProc on
// one thread, B's on another) proves the cross-clock SPSC boundary is race-free (TSan).
#include "aiudio/graph/cross_clock_bridge.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/multi_source_manager.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/io/mock_backend.hpp"
#include "aiudio/io/types.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;
using aiudio::io::MockBackend;
using aiudio::io::StreamConfig;

namespace {
std::unique_ptr<Graph> gainGraph(float g) {
    auto graph = std::make_unique<Graph>();
    const NodeId s = graph->addNode(std::make_unique<SourceNode>(0));
    const NodeId gn = graph->addNode(std::make_unique<GainNode>(g));
    const NodeId k = graph->addNode(std::make_unique<SinkNode>(0));
    graph->connect(s, 0, gn, 0);
    graph->connect(gn, 0, k, 0);
    return graph;
}
StreamConfig cfg(std::uint32_t inCh, std::uint32_t outCh, std::uint32_t block) {
    StreamConfig c;
    c.inputChannels = inCh;
    c.outputChannels = outCh;
    c.blockSize = block;
    return c;
}
constexpr double kRe = 48000.0;
}  // namespace

// A DC level captured by the master device A flows through the graph and across the clock
// boundary to the output device B (resampling preserves DC).
AIUDIO_TEST(dc_flows_across_clocks) {
    auto graph = gainGraph(0.5f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*graph, 1, kRe, 64));
    MultiSourceManager mgr(1, 1, 1, 64, 512);
    CrossClockBridge bridge(mgr, exec, 0, 0, kRe, /*outRate*/ kRe, 1, 8192, 64);

    MockBackend a;  // input device = master clock
    REQUIRE(a.open(cfg(1, 0, 64), &bridge.master()));
    a.setInputValue(0.6f);
    REQUIRE(a.start());
    MockBackend b;  // output device = separate clock
    REQUIRE(b.open(cfg(0, 1, 64), &bridge.output()));
    REQUIRE(b.start());

    for (int i = 0; i < 200; ++i) {  // prime the cross-clock ring, then settle
        a.tick(64);
        b.tick(64);
    }
    CHECK(std::fabs(b.capturedOutput(0) - 0.3f) < 1e-3f);  // 0.6 → gain 0.5 → 0.3
}

// Output device clock runs SLOW: the engine produces faster than B consumes, so the bridge
// ring would fill — the servo speeds B's consumption (ratio up) and the ring stays bounded.
AIUDIO_TEST(slow_output_clock_no_overrun) {
    auto graph = gainGraph(1.0f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*graph, 1, kRe, 64));
    MultiSourceManager mgr(1, 1, 1, 64, 512);
    const std::uint32_t ring = 8192;
    CrossClockBridge bridge(mgr, exec, 0, 0, kRe, kRe, 1, ring, 64);

    MockBackend a, b;
    REQUIRE(a.open(cfg(1, 0, 64), &bridge.master()));
    REQUIRE(b.open(cfg(0, 1, 64), &bridge.output()));
    a.setInputValue(0.4f);
    a.start();
    b.start();

    double bAcc = 0.0;
    std::uint32_t maxFill = 0;
    for (int step = 0; step < 60000; ++step) {
        a.tick(64);
        bAcc += 64.0 * 0.998;  // B clock 0.2% slow → ticks slightly less often
        while (bAcc >= 64.0) {
            b.tick(64);
            bAcc -= 64.0;
        }
        if (step > 4000 && bridge.outputFill() > maxFill) maxFill = bridge.outputFill();
    }
    CHECK(bridge.outputOverruns() == 0);  // ring never overflowed
    CHECK(maxFill < ring);                // stayed inside the ring
    CHECK(bridge.outputRatio() > 1.0);    // servo sped consumption up to match
}

// Output device clock runs FAST: B consumes faster than the engine produces → the ring tends
// to empty; the servo slows B's consumption (ratio down) so starvation stops after warm-up.
AIUDIO_TEST(fast_output_clock_settles) {
    auto graph = gainGraph(1.0f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*graph, 1, kRe, 64));
    MultiSourceManager mgr(1, 1, 1, 64, 512);
    CrossClockBridge bridge(mgr, exec, 0, 0, kRe, kRe, 1, 8192, 64);

    MockBackend a, b;
    REQUIRE(a.open(cfg(1, 0, 64), &bridge.master()));
    REQUIRE(b.open(cfg(0, 1, 64), &bridge.output()));
    a.setInputValue(0.4f);
    a.start();
    b.start();

    auto run = [&](int steps) {
        double bAcc = 0.0;
        for (int s = 0; s < steps; ++s) {
            a.tick(64);
            bAcc += 64.0 * 1.002;  // B clock 0.2% fast
            while (bAcc >= 64.0) {
                b.tick(64);
                bAcc -= 64.0;
            }
        }
    };
    run(40000);
    const std::uint64_t under0 = bridge.outputUnderruns();
    run(8000);
    CHECK(bridge.outputUnderruns() == under0);  // no new starvation after convergence
    CHECK(bridge.outputRatio() < 1.0);          // servo slowed consumption
}

// Acceptance: A's IOProc on one thread, B's IOProc on another — the cross-clock SPSC ring is
// race-free (run under TSan). B's clock drifts slow relative to A's.
AIUDIO_TEST(multithreaded_cross_clock_is_race_free) {
    auto graph = gainGraph(0.5f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*graph, 1, kRe, 64));
    MultiSourceManager mgr(1, 1, 1, 64, 1024);
    CrossClockBridge bridge(mgr, exec, 0, 0, kRe, kRe, 1, 16384, 64);

    MockBackend a, b;
    REQUIRE(a.open(cfg(1, 0, 64), &bridge.master()));
    REQUIRE(b.open(cfg(0, 1, 64), &bridge.output()));
    a.setInputValue(0.5f);
    a.start();
    b.start();

    std::atomic<bool> stop{false};
    std::atomic<long> aTicks{0};
    std::atomic<bool> bad{false};

    std::thread devA([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            a.tick(64);
            aTicks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread devB([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            b.tick(64);
            if (b.capturedOutput(0) < -1.5f || b.capturedOutput(0) > 1.5f) bad.store(true);
        }
    });
    std::thread monitor([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            volatile double r = bridge.outputRatio();
            volatile std::uint32_t f = bridge.outputFill();
            (void)r; (void)f;
        }
    });

    while (aTicks.load(std::memory_order_relaxed) < 4000) { /* run */ }
    stop.store(true, std::memory_order_relaxed);
    devA.join();
    devB.join();
    monitor.join();

    CHECK(!bad.load());
    CHECK(aTicks.load() > 0);
}

AIUDIO_TEST_MAIN()

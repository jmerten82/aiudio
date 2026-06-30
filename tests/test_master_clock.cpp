// Tests for the live path: a MockBackend (M9.4) drives the MasterClockAdapter, which feeds
// the MultiSourceManager + multi-stream graph. Covers the flow, hot-unplug (clean + counted
// + surfaced), device xruns, reconnect, and a multithreaded run (TSan).
#include "aiudio/graph/master_clock_adapter.hpp"

#include <atomic>
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
bool near(float a, float b) { return (a > b ? a - b : b - a) < 1e-6f; }

// source(0) -> gain(g) -> sink(0)
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
}  // namespace

// A mock device's clock drives the adapter → manager → graph → back to the device's output.
AIUDIO_TEST(live_path_flows_through_manager) {
    auto graph = gainGraph(0.5f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*graph, 1, 48000.0, 64));
    MultiSourceManager mgr(1, 1, 1, 64, 256);
    MasterClockAdapter adapter(mgr, exec, 0, 0);

    MockBackend dev;
    REQUIRE(dev.open(cfg(1, 1, 64), &adapter));
    dev.setInputValue(0.5f);
    REQUIRE(dev.start());
    dev.tick(32);  // capture 0.5 → push → pump → gain 0.5 → 0.25 → pop → device out
    CHECK(near(dev.capturedOutput(0), 0.25f));
}

// Hot-unplug: clean (no crash), surfaced (handler fires), and the device stops ticking.
AIUDIO_TEST(disconnect_is_clean_and_surfaced) {
    auto graph = gainGraph(1.0f);
    GraphExecutor exec;
    REQUIRE(exec.compile(*graph, 1, 48000.0, 64));
    MultiSourceManager mgr(1, 1, 1, 64, 256);
    MasterClockAdapter adapter(mgr, exec, 0, 0);

    MockBackend dev;
    REQUIRE(dev.open(cfg(1, 1, 64), &adapter));
    int notified = 0;
    dev.setDisconnectHandler([&] { ++notified; });
    dev.setInputValue(1.0f);
    REQUIRE(dev.start());
    dev.tick(32);
    CHECK(near(dev.capturedOutput(0), 1.0f));

    dev.injectDisconnect();
    CHECK(dev.disconnected());
    CHECK(notified == 1);
    CHECK(!dev.running());
    dev.tick(32);  // no-op, no crash

    // Re-plug recovers.
    dev.injectReconnect();
    REQUIRE(dev.start());
    dev.setInputValue(0.25f);
    dev.tick(32);
    CHECK(near(dev.capturedOutput(0), 0.25f));
}

// Device-side xruns are counted (the engine can't see these; the device reports them).
AIUDIO_TEST(device_xruns_counted) {
    MockBackend dev;
    REQUIRE(dev.open(cfg(1, 1, 64), nullptr) == false);  // null callback rejected
    CHECK(dev.xrunCount() == 0);
    dev.injectXrun(64);
    dev.injectXrun(32);
    CHECK(dev.xrunCount() == 96);
}

// Acceptance: the master clock ticks on one thread while another producer feeds a second
// input stream and a consumer drains a second output stream. Race-free (run under TSan).
AIUDIO_TEST(multithreaded_master_clock_is_race_free) {
    auto graph = std::make_unique<Graph>();
    const NodeId s0 = graph->addNode(std::make_unique<SourceNode>(0));
    const NodeId s1 = graph->addNode(std::make_unique<SourceNode>(1));
    const NodeId k0 = graph->addNode(std::make_unique<SinkNode>(0));
    const NodeId k1 = graph->addNode(std::make_unique<SinkNode>(1));
    graph->connect(s0, 0, k0, 0);
    graph->connect(s1, 0, k1, 0);
    GraphExecutor exec;
    REQUIRE(exec.compile(*graph, 1, 48000.0, 64));
    MultiSourceManager mgr(2, 2, 1, 64, 512);
    MasterClockAdapter adapter(mgr, exec, /*inStream*/ 0, /*outStream*/ 0);

    MockBackend dev;
    REQUIRE(dev.open(cfg(1, 1, 64), &adapter));
    dev.setInputValue(0.5f);
    REQUIRE(dev.start());

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> ticks{0};
    std::atomic<bool> bad{false};

    std::thread clock([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            dev.tick(64);  // drives the pump + stream 0
            ticks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread producer([&] {
        std::vector<float> b(64, 0.3f);
        float* p[1] = {b.data()};
        aiudio::io::AudioBuffer src{p, 1, 64};
        while (!stop.load(std::memory_order_relaxed)) mgr.pushInput(1, src, 64);  // 2nd source
    });
    std::thread consumer([&] {
        std::vector<float> b(64, 0.0f);
        while (!stop.load(std::memory_order_relaxed)) {
            float* p[1] = {b.data()};
            aiudio::io::AudioBuffer dst{p, 1, 64};
            mgr.popOutput(1, dst, 64);  // 2nd sink
            for (float v : b)
                if (v < -0.0001f || v > 1.0001f) bad.store(true, std::memory_order_relaxed);
        }
    });

    while (ticks.load(std::memory_order_relaxed) < 2000) { /* run */ }
    stop.store(true, std::memory_order_relaxed);
    clock.join();
    producer.join();
    consumer.join();

    CHECK(!bad.load());
    CHECK(ticks.load() > 0);
}

AIUDIO_TEST_MAIN()

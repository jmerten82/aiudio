// Tests for live graph edits via atomic schedule swap (G5).
#include "aiudio/graph/graph_executor.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
constexpr std::uint32_t kN = 128;

// Source -> Gain(gain) -> Sink.
std::unique_ptr<Graph> makeGainGraph(float gain) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    const NodeId gn = g->addNode(std::make_unique<GainNode>(gain));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, gn, 0);
    g->connect(gn, 0, k, 0);
    return g;
}

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
}  // namespace

AIUDIO_TEST(swap_takes_effect_and_reclaims) {
    std::vector<std::unique_ptr<Graph>> graphs;  // keep all graphs alive
    GraphExecutor exec;

    graphs.push_back(makeGainGraph(0.5f));
    REQUIRE(exec.compile(*graphs.back(), 1, 48000.0, kN));
    CHECK(std::fabs(runConstant(exec, 1.0f) - 0.5f) < 1e-6f);

    graphs.push_back(makeGainGraph(0.25f));
    REQUIRE(exec.compile(*graphs.back(), 1, 48000.0, kN));  // atomic swap
    CHECK(std::fabs(runConstant(exec, 1.0f) - 0.25f) < 1e-6f);  // new schedule is live

    // A few more swaps; each process() advances renderCount so retired schedules
    // get reclaimed — the pending set stays bounded (doesn't grow per edit).
    for (int i = 0; i < 8; ++i) {
        runConstant(exec, 1.0f);
        graphs.push_back(makeGainGraph(0.1f * static_cast<float>(i + 1)));
        REQUIRE(exec.compile(*graphs.back(), 1, 48000.0, kN));
    }
    CHECK(exec.pendingRetired() <= 1);
}

// The acceptance test: edit the graph repeatedly while another thread runs
// process() in a tight loop. Must be race-free (run under TSan) with no garbage.
AIUDIO_TEST(live_swap_is_race_free) {
    std::vector<std::unique_ptr<Graph>> graphs;
    GraphExecutor exec;
    graphs.push_back(makeGainGraph(0.5f));
    REQUIRE(exec.compile(*graphs.back(), 1, 48000.0, kN));

    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};
    std::atomic<std::uint64_t> blocks{0};

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
            if (!std::isfinite(v) || std::fabs(v) > 1.0001f) bad.store(true, std::memory_order_relaxed);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Control thread: swap in ~1000 new graphs while audio runs.
    for (int i = 0; i < 1000; ++i) {
        graphs.push_back(makeGainGraph(0.1f + 0.0005f * static_cast<float>(i % 1000)));
        exec.compile(*graphs.back(), 1, 48000.0, kN);
    }
    while (blocks.load(std::memory_order_relaxed) < 100) { /* let audio run a bit */ }
    stop.store(true, std::memory_order_relaxed);
    audio.join();

    CHECK(!bad.load());            // never observed garbage output
    CHECK(blocks.load() > 0);      // the audio thread actually ran
}

AIUDIO_TEST_MAIN()

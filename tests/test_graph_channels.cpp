// Tests for per-port channel counts (G8): a node may change the channel width, and the
// executor sizes each port's buffer to the width channelLayout() declares — verified with
// a down-mix (2->1) and an up-mix (1->2), plus the uniform case (back-compatible).
#include "aiudio/graph/graph_executor.hpp"

#include <cstdint>
#include <memory>
#include <utility>

#include "aiudio/graph/downmix_node.hpp"
#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/upmix_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
constexpr std::uint32_t kN = 32;

// Feed a stereo block (left=l, right=r) through a 2-channel-compiled graph; return the
// first sample of each output channel.
std::pair<float, float> runStereo(GraphExecutor& exec, float l, float r) {
    float inL[kN], inR[kN], outL[kN], outR[kN];
    for (std::uint32_t i = 0; i < kN; ++i) {
        inL[i] = l; inR[i] = r; outL[i] = -999.0f; outR[i] = -999.0f;
    }
    float* ic[2] = {inL, inR};
    float* oc[2] = {outL, outR};
    AudioBuffer ib{ic, 2, kN};
    AudioBuffer ob{oc, 2, kN};
    exec.process(ib, ob, kN, TimeInfo{});
    return {outL[0], outR[0]};
}

bool near(float a, float b) { return (a > b ? a - b : b - a) < 1e-6f; }
}  // namespace

// Down-mix collapses stereo to one channel: the sink (2-ch) gets the mono in channel 0 and
// silence in channel 1 — proving the interior port became 1-channel.
AIUDIO_TEST(downmix_two_to_one) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    const NodeId d = g->addNode(std::make_unique<DownmixNode>());
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, d, 0);
    g->connect(d, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 2, 48000.0, kN));   // host = stereo
    const auto [c0, c1] = runStereo(exec, 1.0f, 0.5f);
    CHECK(near(c0, 0.75f));   // (1.0 + 0.5) / 2
    CHECK(near(c1, 0.0f));    // ch1 silent → the signal was mono at the downmix
}

// Up-mix raises a mono signal back to stereo (duplicate): after down->up, both output
// channels carry the same mono value.
AIUDIO_TEST(upmix_one_to_two_after_downmix) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    const NodeId d = g->addNode(std::make_unique<DownmixNode>());
    const NodeId u = g->addNode(std::make_unique<UpmixNode>(2));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, d, 0);
    g->connect(d, 0, u, 0);
    g->connect(u, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 2, 48000.0, kN));
    const auto [c0, c1] = runStereo(exec, 1.0f, 0.5f);
    CHECK(near(c0, 0.75f));   // mono duplicated to both channels
    CHECK(near(c1, 0.75f));
}

// Uniform-width graph (no channel-changing node) is unchanged: per-channel gain, both
// channels preserved (the back-compatible special case where every port = host width).
AIUDIO_TEST(uniform_width_unchanged) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    const NodeId gn = g->addNode(std::make_unique<GainNode>(0.5f));
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, gn, 0);
    g->connect(gn, 0, k, 0);

    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 2, 48000.0, kN));
    const auto [c0, c1] = runStereo(exec, 1.0f, 0.5f);
    CHECK(near(c0, 0.5f));    // L * 0.5
    CHECK(near(c1, 0.25f));   // R * 0.5  → both channels independent, preserved
}

AIUDIO_TEST_MAIN()

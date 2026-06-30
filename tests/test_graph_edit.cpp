// Tests for graph editing: disconnect() + removeNode() tombstoning (stable NodeIds) and that
// the executor compiles + runs correctly after edits. Backs the Python graph-edit bindings.
#include "aiudio/graph/graph.hpp"

#include <memory>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
bool near(float a, float b) { return (a > b ? a - b : b - a) < 1e-5f; }
}  // namespace

// disconnect() removes exactly the matching edge and returns whether one was removed.
AIUDIO_TEST(disconnect_removes_matching_edge) {
    Graph g;
    const NodeId s = g.addNode(std::make_unique<SourceNode>());
    const NodeId gn = g.addNode(std::make_unique<GainNode>(0.5f));
    const NodeId k = g.addNode(std::make_unique<SinkNode>());
    g.connect(s, 0, gn, 0);
    g.connect(gn, 0, k, 0);
    CHECK(g.edges().size() == 2);

    CHECK(g.disconnect(gn, 0, k, 0));        // removes the second edge
    CHECK(g.edges().size() == 1);
    CHECK(!g.disconnect(gn, 0, k, 0));       // already gone → false
    CHECK(g.edges().size() == 1);
}

// removeNode() tombstones the slot: the id stays valid-range but node(id) is null, other
// ids are unchanged, and every edge touching it is dropped.
AIUDIO_TEST(remove_node_tombstones_and_keeps_ids_stable) {
    Graph g;
    const NodeId s = g.addNode(std::make_unique<SourceNode>());
    const NodeId gn = g.addNode(std::make_unique<GainNode>(0.5f));
    const NodeId k = g.addNode(std::make_unique<SinkNode>());
    g.connect(s, 0, gn, 0);
    g.connect(gn, 0, k, 0);
    REQUIRE(g.nodeCount() == 3);
    REQUIRE(g.liveNodeCount() == 3);

    CHECK(g.removeNode(gn));
    CHECK(g.node(gn) == nullptr);            // tombstoned
    CHECK(g.node(s) != nullptr);             // other ids unchanged
    CHECK(g.node(k) != nullptr);
    CHECK(g.nodeCount() == 3);               // slot kept (ids never shift)
    CHECK(g.liveNodeCount() == 2);
    CHECK(g.edges().empty());                // both edges touched gn → dropped

    CHECK(!g.removeNode(gn));                 // already removed → false
    CHECK(!g.removeNode(999));                // out of range → false
}

// connect() rejects edges to a removed node (so a tombstone can't be re-wired by id).
AIUDIO_TEST(connect_rejects_removed_node) {
    Graph g;
    const NodeId s = g.addNode(std::make_unique<SourceNode>());
    const NodeId gn = g.addNode(std::make_unique<GainNode>(0.5f));
    g.removeNode(gn);
    CHECK(!g.connect(s, 0, gn, 0));
    CHECK(g.edges().empty());
}

// The executor compiles + runs correctly after an edit: remove a node, rewire around the
// hole, recompile, and the surviving path renders. Tombstoned slots are skipped in build().
AIUDIO_TEST(executor_runs_after_remove_and_rewire) {
    Graph g;
    const NodeId s = g.addNode(std::make_unique<SourceNode>());
    const NodeId atten = g.addNode(std::make_unique<GainNode>(0.5f));  // will be removed
    const NodeId boost = g.addNode(std::make_unique<GainNode>(2.0f));
    const NodeId k = g.addNode(std::make_unique<SinkNode>());
    g.connect(s, 0, atten, 0);
    g.connect(atten, 0, boost, 0);
    g.connect(boost, 0, k, 0);

    // Remove the attenuator and wire source straight into the booster.
    CHECK(g.removeNode(atten));
    CHECK(g.connect(s, 0, boost, 0));
    CHECK(g.validate().ok);

    GraphExecutor exec;
    REQUIRE(exec.compile(g, 1, 48000.0, 16));

    float in[16], out[16];
    for (float& v : in) v = 1.0f;
    float* ip[1] = {in};
    float* op[1] = {out};
    aiudio::io::AudioBuffer ib{ip, 1, 16};
    aiudio::io::AudioBuffer ob{op, 1, 16};
    exec.process(ib, ob, 16, aiudio::io::TimeInfo{});
    CHECK(near(out[0], 2.0f));   // source → boost(2.0) only; the removed 0.5 atten is gone
}

AIUDIO_TEST_MAIN()

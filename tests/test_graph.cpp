// Tests for the graph IR: construction + validation (G1).
#include "aiudio/graph/graph.hpp"

#include <memory>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

AIUDIO_TEST(valid_dag_is_accepted) {
    Graph g;
    const NodeId a = g.addNode(std::make_unique<GainNode>(0.5f));
    const NodeId b = g.addNode(std::make_unique<GainNode>(0.8f));
    const NodeId mix = g.addNode(std::make_unique<SumNode>(2));
    CHECK(g.connect(a, 0, mix, 0));
    CHECK(g.connect(b, 0, mix, 1));
    const auto r = g.validate();
    CHECK(r.ok);
    CHECK(g.nodeCount() == 3);
    CHECK(g.edges().size() == 2);
}

AIUDIO_TEST(connect_rejects_unknown_node) {
    Graph g;
    const NodeId a = g.addNode(std::make_unique<GainNode>());
    CHECK(!g.connect(a, 0, 999, 0));   // dst doesn't exist
    CHECK(g.edges().empty());
}

AIUDIO_TEST(validate_rejects_out_of_range_port) {
    Graph g;
    const NodeId a = g.addNode(std::make_unique<GainNode>());  // 1 in / 1 out
    const NodeId b = g.addNode(std::make_unique<GainNode>());
    CHECK(g.connect(a, 3, b, 0));      // srcPort 3 >= 1 output → recorded, caught by validate
    const auto r = g.validate();
    CHECK(!r.ok);
}

AIUDIO_TEST(validate_rejects_input_port_with_two_drivers) {
    Graph g;
    const NodeId a = g.addNode(std::make_unique<GainNode>());
    const NodeId b = g.addNode(std::make_unique<GainNode>());
    const NodeId mix = g.addNode(std::make_unique<SumNode>(2));
    CHECK(g.connect(a, 0, mix, 0));
    CHECK(g.connect(b, 0, mix, 0));    // second edge into the same input port → invalid
    const auto r = g.validate();
    CHECK(!r.ok);
}

AIUDIO_TEST(validate_rejects_cycle) {
    Graph g;
    const NodeId a = g.addNode(std::make_unique<GainNode>());
    const NodeId b = g.addNode(std::make_unique<GainNode>());
    CHECK(g.connect(a, 0, b, 0));
    CHECK(g.connect(b, 0, a, 0));      // a → b → a
    const auto r = g.validate();
    CHECK(!r.ok);
}

AIUDIO_TEST_MAIN()

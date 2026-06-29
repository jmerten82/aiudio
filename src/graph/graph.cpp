#include "aiudio/graph/graph.hpp"

#include <string>
#include <vector>

namespace aiudio::graph {

NodeId Graph::addNode(std::unique_ptr<Node> node) {
    nodes_.push_back(std::move(node));
    return static_cast<NodeId>(nodes_.size() - 1);
}

bool Graph::connect(NodeId src, std::uint32_t srcPort, NodeId dst, std::uint32_t dstPort) {
    if (src >= nodes_.size() || dst >= nodes_.size()) return false;  // unknown node
    edges_.push_back(Edge{src, srcPort, dst, dstPort});
    return true;
}

Node* Graph::node(NodeId id) const {
    return (id < nodes_.size()) ? nodes_[id].get() : nullptr;
}

namespace {

// Depth-first back-edge detection over the node-level adjacency (3-colour DFS).
bool hasCycleFrom(NodeId n, const std::vector<std::vector<NodeId>>& adj,
                  std::vector<int>& colour) {
    colour[n] = 1;  // gray (on the stack)
    for (NodeId m : adj[n]) {
        if (colour[m] == 1) return true;                       // back edge → cycle
        if (colour[m] == 0 && hasCycleFrom(m, adj, colour)) return true;
    }
    colour[n] = 2;  // black (done)
    return false;
}

}  // namespace

ValidationResult Graph::validate() const {
    const std::size_t n = nodes_.size();

    // 1) Every edge's endpoints + ports must be in range.
    for (const Edge& e : edges_) {
        if (e.src >= n || e.dst >= n) {
            return {false, "edge references a non-existent node"};
        }
        if (e.srcPort >= nodes_[e.src]->numOutputs()) {
            return {false, std::string("output port out of range on ") +
                               nodes_[e.src]->typeName()};
        }
        if (e.dstPort >= nodes_[e.dst]->numInputs()) {
            return {false, std::string("input port out of range on ") +
                               nodes_[e.dst]->typeName()};
        }
    }

    // 2) Each input port may have at most one incoming edge (mix via a SumNode).
    for (std::size_t i = 0; i < edges_.size(); ++i) {
        for (std::size_t j = i + 1; j < edges_.size(); ++j) {
            if (edges_[i].dst == edges_[j].dst && edges_[i].dstPort == edges_[j].dstPort) {
                return {false, std::string("input port driven by more than one edge on ") +
                                   nodes_[edges_[i].dst]->typeName()};
            }
        }
    }

    // 3) The graph must be acyclic (a DAG).
    std::vector<std::vector<NodeId>> adj(n);
    for (const Edge& e : edges_) adj[e.src].push_back(e.dst);
    std::vector<int> colour(n, 0);  // 0 white, 1 gray, 2 black
    for (NodeId v = 0; v < n; ++v) {
        if (colour[v] == 0 && hasCycleFrom(v, adj, colour)) {
            return {false, "graph contains a cycle"};
        }
    }

    return {true, {}};
}

}  // namespace aiudio::graph

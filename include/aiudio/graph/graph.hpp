// aiudio-graph — the typed graph IR (graph spine, G1 / ADR-0009).
//
// A directed graph of Nodes connected by Edges (srcNode:outPort -> dstNode:inPort).
// This is construction + validation only; execution (compile to a schedule + run)
// is G2. Nodes are owned by the Graph and referenced by NodeId (their insertion
// index).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aiudio/graph/node.hpp"

namespace aiudio::graph {

using NodeId = std::uint32_t;
inline constexpr NodeId kInvalidNode = 0xFFFFFFFFu;

/// A connection: srcNode's output port `srcPort` -> dstNode's input port `dstPort`.
struct Edge {
    NodeId src = kInvalidNode;
    std::uint32_t srcPort = 0;
    NodeId dst = kInvalidNode;
    std::uint32_t dstPort = 0;
};

/// Result of Graph::validate(). `ok == false` carries a human-readable reason.
struct ValidationResult {
    bool ok = true;
    std::string error;  // empty iff ok
    explicit operator bool() const noexcept { return ok; }
};

class Graph {
public:
    Graph() = default;
    // Owns its nodes via unique_ptr → non-copyable, movable.
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) = default;
    Graph& operator=(Graph&&) = default;

    /// Take ownership of a node; returns its id (its insertion index).
    NodeId addNode(std::unique_ptr<Node> node);

    /// Record an edge. Returns false if either node id does not exist or has been
    /// removed (tombstoned); structural validity (port ranges, single-driver, acyclicity)
    /// is validate()'s job, so an otherwise-invalid graph can be built and then diagnosed.
    bool connect(NodeId src, std::uint32_t srcPort, NodeId dst, std::uint32_t dstPort);

    /// Remove the edge matching exactly (src:srcPort -> dst:dstPort). Returns true if one
    /// was removed (false if no such edge existed). A graph edit — recompile to apply.
    bool disconnect(NodeId src, std::uint32_t srcPort, NodeId dst, std::uint32_t dstPort);

    /// Remove a node and every edge touching it. The slot is **tombstoned**, not compacted:
    /// `id` stays valid (NodeIds and other nodes' ids never shift or get reused), and
    /// `node(id)` then returns nullptr. Returns false if `id` is invalid or already removed.
    /// A graph edit — recompile to apply. (Edits are add/remove only; no in-place mutation.)
    bool removeNode(NodeId id);

    /// Check the graph is well-formed: every edge's ports are in range, each input
    /// port has at most one incoming edge (use a SumNode to mix), and the graph is
    /// acyclic (a DAG). Returns ok, or the first problem found. Tombstoned slots are skipped.
    [[nodiscard]] ValidationResult validate() const;

    /// Number of node slots, **including tombstoned (removed) ones** — so it is also the
    /// upper bound on valid NodeIds. Use `node(id) != nullptr` to test liveness.
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
    /// Count of live (non-removed) nodes.
    [[nodiscard]] std::size_t liveNodeCount() const noexcept;
    [[nodiscard]] const std::vector<Edge>& edges() const noexcept { return edges_; }
    [[nodiscard]] Node* node(NodeId id) const;  // nullptr if id is out of range or removed

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<Edge> edges_;
};

}  // namespace aiudio::graph

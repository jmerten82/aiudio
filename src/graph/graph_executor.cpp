#include "aiudio/graph/graph_executor.hpp"

#include <cstddef>
#include <vector>

#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"

namespace aiudio::graph {

bool GraphExecutor::compile(const Graph& g, std::uint32_t numChannels, double sampleRate,
                            std::uint32_t maxBlock) {
    if (!g.validate()) return false;
    if (numChannels == 0 || maxBlock == 0) return false;

    compiled_ = false;
    channels_ = numChannels;
    maxBlock_ = maxBlock;
    storage_.clear();
    ptrs_.clear();
    portBuffers_.clear();
    schedule_.clear();
    sources_.clear();
    sinks_.clear();

    const std::size_t n = g.nodeCount();

    // --- 1) Assign a buffer index to each (node, output port), + one zero buffer.
    std::vector<std::vector<std::size_t>> outBuf(n);  // outBuf[node][port] -> buffer index
    std::size_t bufferCount = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t outs = g.node(static_cast<NodeId>(i))->numOutputs();
        outBuf[i].resize(outs);
        for (std::uint32_t p = 0; p < outs; ++p) outBuf[i][p] = bufferCount++;
    }
    const std::size_t zeroBuf = bufferCount++;  // shared read-only zero buffer

    // --- 2) Allocate all buffers up front (no later resize → stable pointers).
    storage_.assign(bufferCount, std::vector<float>(static_cast<std::size_t>(channels_) * maxBlock_, 0.0f));
    ptrs_.assign(bufferCount, std::vector<float*>(channels_, nullptr));
    portBuffers_.resize(bufferCount);
    for (std::size_t b = 0; b < bufferCount; ++b) {
        for (std::uint32_t c = 0; c < channels_; ++c) ptrs_[b][c] = storage_[b].data() + static_cast<std::size_t>(c) * maxBlock_;
        portBuffers_[b] = io::AudioBuffer{ptrs_[b].data(), channels_, maxBlock_};
    }

    // --- 3) Topological order (Kahn's) over node-level edges.
    std::vector<std::vector<NodeId>> adj(n);
    std::vector<int> indeg(n, 0);
    for (const Edge& e : g.edges()) {
        adj[e.src].push_back(e.dst);
        ++indeg[e.dst];
    }
    std::vector<NodeId> order;
    order.reserve(n);
    std::vector<NodeId> ready;
    for (NodeId v = 0; v < n; ++v) {
        if (indeg[v] == 0) ready.push_back(v);
    }
    while (!ready.empty()) {
        const NodeId v = ready.back();
        ready.pop_back();
        order.push_back(v);
        for (NodeId m : adj[v]) {
            if (--indeg[m] == 0) ready.push_back(m);
        }
    }
    if (order.size() != n) return false;  // cycle (validate should have caught it)

    // --- 4) Build the schedule: per node, resolve input/output buffer views.
    schedule_.reserve(n);
    for (NodeId v : order) {
        Node* node = g.node(v);
        ScheduleEntry entry;
        entry.node = node;

        const std::uint32_t ins = node->numInputs();
        entry.inputs.resize(ins);
        for (std::uint32_t p = 0; p < ins; ++p) {
            std::size_t buf = zeroBuf;  // default: unconnected → zeros
            for (const Edge& e : g.edges()) {
                if (e.dst == v && e.dstPort == p) { buf = outBuf[e.src][e.srcPort]; break; }
            }
            entry.inputs[p] = portBuffers_[buf];
        }

        const std::uint32_t outs = node->numOutputs();
        entry.outputs.resize(outs);
        for (std::uint32_t p = 0; p < outs; ++p) entry.outputs[p] = portBuffers_[outBuf[v][p]];

        if (auto* src = dynamic_cast<SourceNode*>(node)) sources_.push_back(src);
        if (auto* snk = dynamic_cast<SinkNode*>(node)) sinks_.push_back(snk);

        node->prepare(sampleRate, maxBlock_);
        schedule_.push_back(std::move(entry));
    }

    compiled_ = true;
    return true;
}

void GraphExecutor::process(const io::AudioBuffer& in, io::AudioBuffer& out,
                            std::uint32_t numFrames, const io::TimeInfo& time) noexcept {
    if (!compiled_) return;
    const std::uint32_t frames = (numFrames <= maxBlock_) ? numFrames : maxBlock_;

    for (SourceNode* s : sources_) s->setExternalInput(&in);
    for (SinkNode* k : sinks_) k->setExternalOutput(&out);

    for (ScheduleEntry& e : schedule_) {
        e.node->process(e.inputs.data(), e.outputs.data(), frames, time);
    }
}

}  // namespace aiudio::graph

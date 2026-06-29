#include "aiudio/graph/graph_executor.hpp"

#include <cstddef>
#include <vector>

#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"

namespace aiudio::graph {

// A fully self-contained compiled schedule: its own buffer storage + views, the
// topological run order, and the source/sink nodes. Swapped atomically as a unit.
struct GraphExecutor::CompiledGraph {
    struct Entry {
        Node* node = nullptr;
        std::vector<io::AudioBuffer> inputs;
        std::vector<io::AudioBuffer> outputs;
    };
    std::vector<std::vector<float>> storage;    // [buffer][channel*maxBlock]
    std::vector<std::vector<float*>> ptrs;       // [buffer][channel]
    std::vector<io::AudioBuffer> portBuffers;    // [buffer] view
    std::vector<Entry> schedule;                 // topological order
    std::vector<Node*> byId;                     // NodeId -> Node* (O(1) command routing)
    std::vector<SourceNode*> sources;
    std::vector<SinkNode*> sinks;
    std::uint32_t maxBlock = 0;
};

GraphExecutor::GraphExecutor()
    : commands_(std::make_unique<io::RingBuffer<ParamCommand>>(kCommandCapacity)) {}

GraphExecutor::~GraphExecutor() {
    retired_.clear();
    delete active_.load(std::memory_order_relaxed);  // assumes the audio thread has stopped
}

std::unique_ptr<GraphExecutor::CompiledGraph> GraphExecutor::build(const Graph& g,
                                                                   std::uint32_t numChannels,
                                                                   double sampleRate,
                                                                   std::uint32_t maxBlock) {
    if (!g.validate() || numChannels == 0 || maxBlock == 0) return nullptr;

    auto cg = std::make_unique<CompiledGraph>();
    cg->maxBlock = maxBlock;
    const std::size_t n = g.nodeCount();
    cg->byId.assign(n, nullptr);  // filled per node below; command drain indexes by NodeId

    // 1) A buffer per (node, output port), + one shared zero buffer.
    std::vector<std::vector<std::size_t>> outBuf(n);
    std::size_t bufferCount = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t outs = g.node(static_cast<NodeId>(i))->numOutputs();
        outBuf[i].resize(outs);
        for (std::uint32_t p = 0; p < outs; ++p) outBuf[i][p] = bufferCount++;
    }
    const std::size_t zeroBuf = bufferCount++;

    // 2) Allocate all buffers up front (no later resize → stable pointers).
    cg->storage.assign(bufferCount,
                       std::vector<float>(static_cast<std::size_t>(numChannels) * maxBlock, 0.0f));
    cg->ptrs.assign(bufferCount, std::vector<float*>(numChannels, nullptr));
    cg->portBuffers.resize(bufferCount);
    for (std::size_t b = 0; b < bufferCount; ++b) {
        for (std::uint32_t c = 0; c < numChannels; ++c)
            cg->ptrs[b][c] = cg->storage[b].data() + static_cast<std::size_t>(c) * maxBlock;
        cg->portBuffers[b] = io::AudioBuffer{cg->ptrs[b].data(), numChannels, maxBlock};
    }

    // 3) Topological order (Kahn).
    std::vector<std::vector<NodeId>> adj(n);
    std::vector<int> indeg(n, 0);
    for (const Edge& e : g.edges()) {
        adj[e.src].push_back(e.dst);
        ++indeg[e.dst];
    }
    std::vector<NodeId> order;
    order.reserve(n);
    std::vector<NodeId> ready;
    for (NodeId v = 0; v < n; ++v)
        if (indeg[v] == 0) ready.push_back(v);
    while (!ready.empty()) {
        const NodeId v = ready.back();
        ready.pop_back();
        order.push_back(v);
        for (NodeId m : adj[v])
            if (--indeg[m] == 0) ready.push_back(m);
    }
    if (order.size() != n) return nullptr;  // cycle

    // 4) Build the schedule, resolving input/output buffer views.
    cg->schedule.reserve(n);
    for (NodeId v : order) {
        Node* node = g.node(v);
        cg->byId[v] = node;
        CompiledGraph::Entry entry;
        entry.node = node;

        const std::uint32_t ins = node->numInputs();
        entry.inputs.resize(ins);
        for (std::uint32_t p = 0; p < ins; ++p) {
            std::size_t buf = zeroBuf;
            for (const Edge& e : g.edges())
                if (e.dst == v && e.dstPort == p) { buf = outBuf[e.src][e.srcPort]; break; }
            entry.inputs[p] = cg->portBuffers[buf];
        }
        const std::uint32_t outs = node->numOutputs();
        entry.outputs.resize(outs);
        for (std::uint32_t p = 0; p < outs; ++p) entry.outputs[p] = cg->portBuffers[outBuf[v][p]];

        if (auto* src = dynamic_cast<SourceNode*>(node)) cg->sources.push_back(src);
        if (auto* snk = dynamic_cast<SinkNode*>(node)) cg->sinks.push_back(snk);

        node->prepare(sampleRate, maxBlock);
        cg->schedule.push_back(std::move(entry));
    }
    return cg;
}

bool GraphExecutor::compile(const Graph& g, std::uint32_t numChannels, double sampleRate,
                            std::uint32_t maxBlock) {
    auto next = build(g, numChannels, sampleRate, maxBlock);
    if (!next) return false;

    // Atomically install the new schedule (release: its contents are visible to the
    // audio thread's acquire-load). The audio thread picks it up next block.
    CompiledGraph* old = active_.exchange(next.release(), std::memory_order_acq_rel);
    if (old != nullptr) {
        // Defer freeing `old`: an in-flight process() may still hold it. It becomes
        // safe once the audio thread completes a block after this swap (renderCount_
        // advances past the value captured now).
        retired_.emplace_back(std::unique_ptr<CompiledGraph>(old),
                              renderCount_.load(std::memory_order_acquire));
    }
    reclaim();
    return true;
}

void GraphExecutor::reclaim() {
    const std::uint64_t done = renderCount_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < retired_.size();) {
        if (done > retired_[i].second) {
            retired_[i] = std::move(retired_.back());  // safe to free now
            retired_.pop_back();
        } else {
            ++i;
        }
    }
}

void GraphExecutor::process(const io::AudioBuffer& in, io::AudioBuffer& out,
                            std::uint32_t numFrames, const io::TimeInfo& time) noexcept {
    CompiledGraph* g = active_.load(std::memory_order_acquire);
    if (g == nullptr) return;
    const std::uint32_t frames = (numFrames <= g->maxBlock) ? numFrames : g->maxBlock;

    // Apply queued control-rate edits before rendering, so this block already reflects
    // them. Bounded by the queue capacity; pop() is wait-free and setParam() is RT-safe.
    ParamCommand cmd;
    while (commands_->pop(cmd)) {
        if (cmd.node < g->byId.size()) {
            Node* target = g->byId[cmd.node];
            if (target != nullptr) target->setParam(cmd.param, cmd.value);
        }
    }

    for (SourceNode* s : g->sources) s->setExternalInput(&in);
    for (SinkNode* k : g->sinks) k->setExternalOutput(&out);
    for (CompiledGraph::Entry& e : g->schedule)
        e.node->process(e.inputs.data(), e.outputs.data(), frames, time);

    renderCount_.fetch_add(1, std::memory_order_release);
}

}  // namespace aiudio::graph

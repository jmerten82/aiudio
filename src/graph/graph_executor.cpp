#include "aiudio/graph/graph_executor.hpp"

#include <cstddef>
#include <vector>

#include "aiudio/graph/delay_line.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"

namespace aiudio::graph {

// A fully self-contained compiled schedule: its own buffer storage + views, the
// topological run order, and the source/sink nodes. Swapped atomically as a unit.
struct GraphExecutor::CompiledGraph {
    // A delay-compensation task on one of a node's input ports (G9): run `line` from the
    // upstream buffer `src` into the delayed buffer `dst` before the node reads `dst`.
    struct Comp {
        std::size_t line = 0;     // index into `compDelays`
        io::AudioBuffer src;      // upstream output buffer view
        io::AudioBuffer dst;      // delayed buffer view (== the node's input view)
    };
    struct Entry {
        Node* node = nullptr;
        std::vector<io::AudioBuffer> inputs;
        std::vector<io::AudioBuffer> outputs;
        std::vector<Comp> comps;                 // delay compensation for under-latent inputs
    };
    std::vector<std::vector<float>> storage;    // [buffer][channel*maxBlock]
    std::vector<std::vector<float*>> ptrs;       // [buffer][channel]
    std::vector<io::AudioBuffer> portBuffers;    // [buffer] view
    std::vector<Entry> schedule;                 // topological order
    std::vector<Node*> byId;                     // NodeId -> Node* (O(1) command routing)
    std::vector<SourceNode*> sources;
    std::vector<SinkNode*> sinks;
    // G9 delay-compensation storage (one delayed buffer + line per compensated input port).
    std::vector<DelayLine> compDelays;
    std::vector<std::vector<float>> compStorage;
    std::vector<std::vector<float*>> compPtrs;
    std::uint32_t maxBlock = 0;
    std::uint32_t numChannels = 0;               // per-port channel count (uniform pre-G8)
    std::uint32_t inputStreams = 0;              // max source streamIndex + 1
    std::uint32_t outputStreams = 0;             // max sink streamIndex + 1
    std::uint32_t latency = 0;                    // total graph latency (frames)
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
    cg->numChannels = numChannels;
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

    // 2) Topological order (Kahn) — computed BEFORE sizing buffers, because a node's
    //    output channel width depends on its (upstream) input widths.
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

    // 3) Channel-width propagation (G8): in topological order, each node maps its input
    //    port widths to its output port widths via channelLayout(). Unconnected inputs
    //    default to the host width; the shared zero buffer is sized to the widest port.
    std::vector<std::vector<std::uint32_t>> outWidth(n);
    for (std::size_t i = 0; i < n; ++i)
        outWidth[i].assign(g.node(static_cast<NodeId>(i))->numOutputs(), 0);
    std::uint32_t maxWidth = numChannels;
    std::vector<std::uint32_t> inW, outW;
    for (NodeId v : order) {
        Node* node = g.node(v);
        const std::uint32_t ins = node->numInputs();
        const std::uint32_t outs = node->numOutputs();
        inW.assign(ins, numChannels);  // unconnected input → host-width zeros
        for (std::uint32_t p = 0; p < ins; ++p)
            for (const Edge& e : g.edges())
                if (e.dst == v && e.dstPort == p) { inW[p] = outWidth[e.src][e.srcPort]; break; }
        outW.assign(outs, 0);
        node->channelLayout(inW.data(), ins, outW.data(), outs, numChannels);
        for (std::uint32_t p = 0; p < outs; ++p) {
            outWidth[v][p] = outW[p];
            if (outW[p] > maxWidth) maxWidth = outW[p];
        }
    }

    // 4) Allocate each buffer at its own channel width (no later resize → stable pointers).
    std::vector<std::uint32_t> bufWidth(bufferCount, numChannels);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t p = 0; p < outBuf[i].size(); ++p) bufWidth[outBuf[i][p]] = outWidth[i][p];
    bufWidth[zeroBuf] = maxWidth;
    cg->storage.resize(bufferCount);
    cg->ptrs.resize(bufferCount);
    cg->portBuffers.resize(bufferCount);
    for (std::size_t b = 0; b < bufferCount; ++b) {
        const std::uint32_t w = bufWidth[b];
        cg->storage[b].assign(static_cast<std::size_t>(w) * maxBlock, 0.0f);
        cg->ptrs[b].assign(w, nullptr);
        for (std::uint32_t c = 0; c < w; ++c)
            cg->ptrs[b][c] = cg->storage[b].data() + static_cast<std::size_t>(c) * maxBlock;
        cg->portBuffers[b] = io::AudioBuffer{cg->ptrs[b].data(), w, maxBlock};
    }

    // 4b) Latency propagation (G9): accumulated per-port latency in topological order, and
    //     the per-input-port compensation needed at fan-ins so branches recombine in phase.
    std::vector<std::vector<std::uint32_t>> outLat(n);
    for (std::size_t i = 0; i < n; ++i)
        outLat[i].assign(g.node(static_cast<NodeId>(i))->numOutputs(), 0);
    std::vector<std::vector<std::uint32_t>> compFrames(n);  // [node][input port]
    for (NodeId v : order) {
        Node* node = g.node(v);
        const std::uint32_t ins = node->numInputs();
        std::vector<std::uint32_t> srcLat(ins, 0);
        std::vector<char> connected(ins, 0);
        for (std::uint32_t p = 0; p < ins; ++p)
            for (const Edge& e : g.edges())
                if (e.dst == v && e.dstPort == p) {
                    srcLat[p] = outLat[e.src][e.srcPort];
                    connected[p] = 1;
                    break;
                }
        std::uint32_t inMax = 0;
        for (std::uint32_t p = 0; p < ins; ++p)
            if (srcLat[p] > inMax) inMax = srcLat[p];
        compFrames[v].assign(ins, 0);
        for (std::uint32_t p = 0; p < ins; ++p)
            if (connected[p] && srcLat[p] < inMax) compFrames[v][p] = inMax - srcLat[p];
        const std::uint32_t nodeLat = inMax + node->latencyFrames();
        for (std::uint32_t p = 0; p < node->numOutputs(); ++p) outLat[v][p] = nodeLat;
        if (node->numOutputs() == 0 && inMax > cg->latency) cg->latency = inMax;  // sink → output latency
    }

    // 5) Build the schedule, resolving input/output buffer views (+ delay compensation).
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
            const std::uint32_t comp = (p < compFrames[v].size()) ? compFrames[v][p] : 0;
            if (comp == 0) {
                entry.inputs[p] = cg->portBuffers[buf];
            } else {
                // Route this under-latent input through a compensating delay line into a
                // dedicated delayed buffer (moving inner vectors keeps their data() stable).
                const std::uint32_t w = bufWidth[buf];
                const std::size_t bi = cg->compStorage.size();
                cg->compStorage.emplace_back(static_cast<std::size_t>(w) * maxBlock, 0.0f);
                cg->compPtrs.emplace_back(w, nullptr);
                for (std::uint32_t c = 0; c < w; ++c)
                    cg->compPtrs[bi][c] = cg->compStorage[bi].data() + static_cast<std::size_t>(c) * maxBlock;
                io::AudioBuffer dst{cg->compPtrs[bi].data(), w, maxBlock};
                const std::size_t li = cg->compDelays.size();
                cg->compDelays.emplace_back();
                cg->compDelays[li].prepare(w, comp);
                entry.inputs[p] = dst;
                entry.comps.push_back(CompiledGraph::Comp{li, cg->portBuffers[buf], dst});
            }
        }
        const std::uint32_t outs = node->numOutputs();
        entry.outputs.resize(outs);
        for (std::uint32_t p = 0; p < outs; ++p) entry.outputs[p] = cg->portBuffers[outBuf[v][p]];

        if (auto* src = dynamic_cast<SourceNode*>(node)) {
            cg->sources.push_back(src);
            if (src->streamIndex() + 1 > cg->inputStreams) cg->inputStreams = src->streamIndex() + 1;
        }
        if (auto* snk = dynamic_cast<SinkNode*>(node)) {
            cg->sinks.push_back(snk);
            if (snk->streamIndex() + 1 > cg->outputStreams) cg->outputStreams = snk->streamIndex() + 1;
        }

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

namespace {
// RT-safe: zero `outputs[*]` for frames [from, to) — the substitution on an under-served
// block (silence, never garbage). M9.1.
void silence(io::AudioBuffer* outputs, std::uint32_t numOutputs, std::uint32_t from,
             std::uint32_t to) noexcept {
    for (std::uint32_t o = 0; o < numOutputs; ++o)
        for (std::uint32_t c = 0; c < outputs[o].numChannels; ++c) {
            float* d = outputs[o].channel(c);
            for (std::uint32_t f = from; f < to; ++f) d[f] = 0.0f;
        }
}
}  // namespace

void GraphExecutor::process(const io::AudioBuffer& in, io::AudioBuffer& out,
                            std::uint32_t numFrames, const io::TimeInfo& time) noexcept {
    // The 1-stream special case: one input, one output (back-compatible).
    process(&in, 1, &out, 1, numFrames, time);
}

void GraphExecutor::process(const io::AudioBuffer* inputs, std::uint32_t numInputs,
                            io::AudioBuffer* outputs, std::uint32_t numOutputs,
                            std::uint32_t numFrames, const io::TimeInfo& time) noexcept {
    CompiledGraph* g = active_.load(std::memory_order_acquire);
    if (g == nullptr) {  // not compiled → produce silence (not stale/garbage), count as xrun
        silence(outputs, numOutputs, 0, numFrames);
        xruns_.fetch_add(1, std::memory_order_release);
        return;
    }
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

    // Route each Source/Sink to its bound stream; out-of-range streams → silence / no-write.
    for (SourceNode* s : g->sources) {
        const std::uint32_t si = s->streamIndex();
        s->setExternalInput(si < numInputs ? &inputs[si] : nullptr);
    }
    for (SinkNode* k : g->sinks) {
        const std::uint32_t si = k->streamIndex();
        k->setExternalOutput(si < numOutputs ? &outputs[si] : nullptr);
    }
    for (CompiledGraph::Entry& e : g->schedule) {
        for (CompiledGraph::Comp& c : e.comps)  // G9: feed under-latent inputs through their delay
            g->compDelays[c.line].process(c.src, c.dst, frames);
        e.node->process(e.inputs.data(), e.outputs.data(), frames, time);
    }

    if (frames < numFrames) {  // requested more than maxBlock → silence the tail, count (M9.1)
        silence(outputs, numOutputs, frames, numFrames);
        xruns_.fetch_add(1, std::memory_order_release);
    }

    renderCount_.fetch_add(1, std::memory_order_release);
}

std::uint32_t GraphExecutor::channels() const noexcept {
    CompiledGraph* g = active_.load(std::memory_order_acquire);
    return g ? g->numChannels : 0;
}

std::uint32_t GraphExecutor::inputStreamCount() const noexcept {
    CompiledGraph* g = active_.load(std::memory_order_acquire);
    return g ? g->inputStreams : 0;
}

std::uint32_t GraphExecutor::outputStreamCount() const noexcept {
    CompiledGraph* g = active_.load(std::memory_order_acquire);
    return g ? g->outputStreams : 0;
}

std::uint32_t GraphExecutor::latencyFrames() const noexcept {
    CompiledGraph* g = active_.load(std::memory_order_acquire);
    return g ? g->latency : 0;
}

}  // namespace aiudio::graph

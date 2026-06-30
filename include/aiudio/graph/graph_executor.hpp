// aiudio-graph — GraphExecutor: compile a Graph into a static topological schedule
// with pre-allocated per-port buffers, and run it as a RenderCallback (G2 /
// ADR-0009). `process()` is real-time-safe (no allocation/locks).
//
// G5 — live edits: `compile()` builds a new compiled schedule off-thread and
// installs it with an **atomic swap**; the audio thread picks it up at the next
// block (never a half-edited graph), and the previous schedule is freed off-thread
// once the audio thread has released it (RCU-style, ADR-0005). Call `compile()`
// from a single control thread; `process()` runs on the audio thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "aiudio/graph/graph.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/ring_buffer.hpp"

namespace aiudio::graph {

/// A control-rate parameter change queued from the control thread (e.g. Python / the
/// agent) and applied by the audio thread at the top of the next block. Trivially
/// copyable so it can ride the lock-free SPSC queue (ADR-0004). `node`/`param` map to
/// a node's `setParam` index (see each node's `kSomething` constants).
struct ParamCommand {
    NodeId node = kInvalidNode;
    std::uint32_t param = 0;
    float value = 0.0f;
};

class GraphExecutor final : public io::RenderCallback {
public:
    GraphExecutor();           // defined in the .cpp (CompiledGraph is incomplete here)
    ~GraphExecutor() override;
    GraphExecutor(const GraphExecutor&) = delete;
    GraphExecutor& operator=(const GraphExecutor&) = delete;

    /// Build a compiled schedule for `g` (numChannels at sampleRate, blocks up to
    /// maxBlock) and atomically install it. Safe to call while `process()` runs on
    /// the audio thread — the new schedule swaps in at the next block, the old one
    /// is reclaimed off-thread. Off-thread/setup (allocates). Returns false if the
    /// graph fails validation. The Graph backing any installed schedule must
    /// outlive the executor's use of it.
    bool compile(const Graph& g, std::uint32_t numChannels, double sampleRate,
                 std::uint32_t maxBlock);

    /// RenderCallback (the **1-stream** entry point, back-compatible): route `in` to every
    /// stream-0 SourceNode and `out` to every stream-0 SinkNode. Equivalent to the
    /// multi-stream form with one input and one output. Real-time-safe.
    void process(const io::AudioBuffer& in, io::AudioBuffer& out, std::uint32_t numFrames,
                 const io::TimeInfo& time) noexcept override;

    /// Multi-stream entry point (G10): drain queued parameter commands, route `inputs[k]`
    /// to SourceNodes bound to stream `k` and `outputs[k]` to SinkNodes bound to stream `k`,
    /// run the active schedule in topological order. A SourceNode whose stream index is
    /// ≥ `numInputs` emits silence; a SinkNode whose stream index is ≥ `numOutputs` writes
    /// nothing. Real-time-safe (no allocation/locks). `inputs`/`outputs` point to
    /// `numInputs`/`numOutputs` AudioBuffers. This is the entry the multi-source manager
    /// (M10) will drive.
    void process(const io::AudioBuffer* inputs, std::uint32_t numInputs, io::AudioBuffer* outputs,
                 std::uint32_t numOutputs, std::uint32_t numFrames,
                 const io::TimeInfo& time) noexcept;

    /// Control thread → audio thread: queue a parameter change for `node`'s parameter
    /// `index`, applied at the top of the next process() block (lock-free SPSC,
    /// ADR-0004 — never blocks the audio thread). Returns false if the queue is full
    /// (the change is dropped; the caller may retry). Call from a single control
    /// thread; NEVER from the audio thread. This is the Python/agent control hook —
    /// it keeps the control plane entirely off the audio thread.
    bool postParam(NodeId node, std::uint32_t index, float value) noexcept {
        return commands_->push(ParamCommand{node, index, value});
    }

    [[nodiscard]] bool compiled() const noexcept {
        return active_.load(std::memory_order_acquire) != nullptr;
    }

    /// Telemetry for off-thread observers (a Python frontend / UI): the number of
    /// blocks the audio thread has processed so far. Monotonic; published by the audio
    /// thread, read by the control thread. A climbing value means audio is flowing.
    [[nodiscard]] std::uint64_t renderCount() const noexcept {
        return renderCount_.load(std::memory_order_acquire);
    }

    /// Compiled channel count per port (0 if not compiled). Uniform across the graph
    /// until per-port channel counts (G8) land.
    [[nodiscard]] std::uint32_t channels() const noexcept;

    /// Number of distinct input / output streams the compiled graph uses (= max bound
    /// stream index + 1 over Source / Sink nodes; 0 if none). Lets a caller size the
    /// `inputs`/`outputs` it passes to the multi-stream `process()`.
    [[nodiscard]] std::uint32_t inputStreamCount() const noexcept;
    [[nodiscard]] std::uint32_t outputStreamCount() const noexcept;

    /// Number of swapped-out schedules awaiting safe reclamation (control-thread
    /// view; for tests/introspection).
    [[nodiscard]] std::size_t pendingRetired() const noexcept { return retired_.size(); }

private:
    struct CompiledGraph;  // defined in the .cpp

    // Bound on parameter commands buffered between audio blocks (lock-free, fixed at
    // construction so no allocation ever happens on the post path or the audio thread).
    static constexpr std::size_t kCommandCapacity = 1024;

    static std::unique_ptr<CompiledGraph> build(const Graph& g, std::uint32_t numChannels,
                                                double sampleRate, std::uint32_t maxBlock);
    void reclaim();  // free retired schedules the audio thread has released (control thread)

    std::atomic<CompiledGraph*> active_{nullptr};   // current schedule (audio thread reads)
    std::atomic<std::uint64_t> renderCount_{0};     // bumped at the end of each process()
    // Heap-held (control thread → audio thread): the ring is cache-line over-aligned to
    // avoid false sharing, so it lives behind a pointer to keep GraphExecutor's own
    // alignment normal (nanobind can't hold an over-aligned instance). Built in the ctor.
    std::unique_ptr<io::RingBuffer<ParamCommand>> commands_;
    std::vector<std::pair<std::unique_ptr<CompiledGraph>, std::uint64_t>> retired_;  // control-thread only
};

}  // namespace aiudio::graph

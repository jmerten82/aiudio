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

namespace aiudio::graph {

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

    /// RenderCallback: route `in` to SourceNodes, run the active schedule in
    /// topological order, SinkNodes write `out`. Real-time-safe.
    void process(const io::AudioBuffer& in, io::AudioBuffer& out, std::uint32_t numFrames,
                 const io::TimeInfo& time) noexcept override;

    [[nodiscard]] bool compiled() const noexcept {
        return active_.load(std::memory_order_acquire) != nullptr;
    }

    /// Number of swapped-out schedules awaiting safe reclamation (control-thread
    /// view; for tests/introspection).
    [[nodiscard]] std::size_t pendingRetired() const noexcept { return retired_.size(); }

private:
    struct CompiledGraph;  // defined in the .cpp

    static std::unique_ptr<CompiledGraph> build(const Graph& g, std::uint32_t numChannels,
                                                double sampleRate, std::uint32_t maxBlock);
    void reclaim();  // free retired schedules the audio thread has released (control thread)

    std::atomic<CompiledGraph*> active_{nullptr};   // current schedule (audio thread reads)
    std::atomic<std::uint64_t> renderCount_{0};     // bumped at the end of each process()
    std::vector<std::pair<std::unique_ptr<CompiledGraph>, std::uint64_t>> retired_;  // control-thread only
};

}  // namespace aiudio::graph

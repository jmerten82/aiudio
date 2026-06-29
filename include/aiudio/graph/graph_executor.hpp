// aiudio-graph — GraphExecutor: compile a Graph into a static topological schedule
// with pre-allocated per-port buffers, and run it as a RenderCallback (G2 /
// ADR-0009). `compile()` is setup-time (allocates); `process()` is real-time-safe
// (no allocation/locks). The executor being a RenderCallback is what lets any I/O
// backend drive a whole graph.
#pragma once

#include <cstdint>
#include <vector>

#include "aiudio/graph/graph.hpp"
#include "aiudio/io/render_callback.hpp"

namespace aiudio::graph {

class SourceNode;
class SinkNode;

class GraphExecutor final : public io::RenderCallback {
public:
    /// Compile `g` for `numChannels` channels at `sampleRate`, blocks up to
    /// `maxBlock` frames. Returns false if the graph fails validation. Not RT-safe.
    bool compile(const Graph& g, std::uint32_t numChannels, double sampleRate,
                 std::uint32_t maxBlock);

    /// RenderCallback: route `in` to SourceNodes, run the schedule in topological
    /// order, and SinkNodes write to `out`. Real-time-safe.
    void process(const io::AudioBuffer& in, io::AudioBuffer& out, std::uint32_t numFrames,
                 const io::TimeInfo& time) noexcept override;

    [[nodiscard]] bool compiled() const noexcept { return compiled_; }

private:
    struct ScheduleEntry {
        Node* node = nullptr;
        std::vector<io::AudioBuffer> inputs;   // one view per input port
        std::vector<io::AudioBuffer> outputs;  // one view per output port
    };

    // Per-port buffer storage. Built fully in compile() and never resized after,
    // so the AudioBuffer views below stay valid.
    std::vector<std::vector<float>> storage_;   // [buffer][channel*maxBlock]
    std::vector<std::vector<float*>> ptrs_;      // [buffer][channel]
    std::vector<io::AudioBuffer> portBuffers_;   // [buffer] view

    std::vector<ScheduleEntry> schedule_;        // topological order
    std::vector<SourceNode*> sources_;
    std::vector<SinkNode*> sinks_;

    std::uint32_t channels_ = 0;
    std::uint32_t maxBlock_ = 0;
    bool compiled_ = false;
};

}  // namespace aiudio::graph

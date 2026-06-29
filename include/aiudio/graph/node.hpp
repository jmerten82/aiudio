// aiudio-graph — the Node contract (graph spine, G1 / ADR-0009).
//
// A Node is a graph-level processor with N input ports + M output ports, each a
// planar float32 AudioBuffer. It generalizes the I/O-boundary RenderCallback
// (1-in/1-out) to the multi-port graph case. `process()` is the real-time render
// and must be RT-safe (ADR-0004: no allocation/locks/exceptions); `prepare()` is
// setup-time and may allocate.
#pragma once

#include <cstdint>

#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/types.hpp"

namespace aiudio::graph {

using io::AudioBuffer;
using io::TimeInfo;

class Node {
public:
    virtual ~Node() = default;

    /// Setup; may allocate. Called once before processing starts.
    virtual void prepare(double sampleRate, std::uint32_t maxBlock) = 0;

    /// Render one block. `inputs` points to `numInputs()` AudioBuffers (one per
    /// input port); fill the `numOutputs()` AudioBuffers in `outputs`. RT-safe.
    virtual void process(const AudioBuffer* inputs, AudioBuffer* outputs,
                         std::uint32_t numFrames, const TimeInfo& time) noexcept = 0;

    [[nodiscard]] virtual std::uint32_t numInputs() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t numOutputs() const noexcept = 0;
    [[nodiscard]] virtual const char* typeName() const noexcept = 0;

    /// Apply a control-rate parameter change (the meaning of `index` is node-defined;
    /// see each node's `kSomething` constants). Called **on the audio thread** by the
    /// executor draining its command queue, so it MUST be real-time-safe (no
    /// allocation/locks/blocking — ADR-0004). Default: no-op (node has no parameters).
    /// The control thread never calls this directly; it enqueues via
    /// `GraphExecutor::postParam` (lock-free SPSC), keeping Python off the audio thread.
    virtual void setParam(std::uint32_t index, float value) noexcept {
        (void)index;
        (void)value;
    }
};

}  // namespace aiudio::graph

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

    /// Channel-count layout (G8, compile-time): given the channel width at each input
    /// port (`inWidths[0..numIn)`), fill the width of each output port (`outWidths[0..numOut)`).
    /// The executor calls this in topological order to size each port's buffer, so widths
    /// can change through the graph (down-mix, up-mix). `hostChannels` is the compiled
    /// graph channel count (used for source/generator nodes with no inputs).
    ///
    /// Default — **inherit / broadcast**: 0-input nodes emit `hostChannels`; every other
    /// node emits `max(inWidths)` on all outputs. This is correct for all channel-agnostic
    /// nodes (gain, sum, meter, biquad, source, sink); only width-*changing* nodes override.
    virtual void channelLayout(const std::uint32_t* inWidths, std::uint32_t numIn,
                               std::uint32_t* outWidths, std::uint32_t numOut,
                               std::uint32_t hostChannels) const noexcept {
        std::uint32_t w = 0;
        for (std::uint32_t i = 0; i < numIn; ++i)
            if (inWidths[i] > w) w = inWidths[i];
        if (w == 0) w = hostChannels;  // no inputs, or all-zero → the host width
        for (std::uint32_t o = 0; o < numOut; ++o) outWidths[o] = w;
    }
};

}  // namespace aiudio::graph

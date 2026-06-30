// aiudio-graph — MultiSourceManager (M10): composes N input sources + M output sinks onto
// one engine clock, realizing ADR-0008 §5's "multi-source manager that owns N backends +
// N ring buffers". Each (stream, channel) crosses the thread boundary through its OWN
// lock-free SPSC ring (ADR-0008 §2): producers call pushInput() from their source thread,
// consumers call popOutput() from their sink thread, and the master clock (a device IOProc)
// or a manual pump calls process() to drain the inputs, run the multi-stream graph (G10),
// and fill the outputs. Everything is wait-free (ADR-0004); mixing/routing is the graph's
// job (ADR-0008 §4). Per-stream xrun telemetry comes from the ring counters (M9.1).
//
// Scope: this composes the streams on ONE clock. Bringing genuinely off-clock device
// sources into sync (drift compensation) is M9.5; here a source thread (or Python) feeds
// each input ring.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/ring_buffer.hpp"
#include "aiudio/io/types.hpp"

namespace aiudio::graph {

class MultiSourceManager {
public:
    /// N input streams + M output streams, each `channels` wide, blocks up to `maxBlock`,
    /// per-(stream,channel) ring holding `ringFrames` frames. Allocates (setup-time).
    MultiSourceManager(std::uint32_t numInputs, std::uint32_t numOutputs, std::uint32_t channels,
                       std::uint32_t maxBlock, std::uint32_t ringFrames);

    // Owns lock-free rings (unique_ptr) → non-copyable. (It is copy-constructible *per the
    // trait* via its vector members but ill-formed when copied, which would break nanobind's
    // class registration — same trap the Graph type hit; make it explicit.)
    MultiSourceManager(const MultiSourceManager&) = delete;
    MultiSourceManager& operator=(const MultiSourceManager&) = delete;

    [[nodiscard]] std::uint32_t numInputs() const noexcept { return numIn_; }
    [[nodiscard]] std::uint32_t numOutputs() const noexcept { return numOut_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

    /// Producer (source thread / Python): write `frames` of planar audio into input
    /// `stream`'s rings. Returns frames accepted (< frames → ring full / overrun). RT-safe.
    std::uint32_t pushInput(std::uint32_t stream, const io::AudioBuffer& src,
                            std::uint32_t frames) noexcept;

    /// Consumer (sink thread / Python): read `frames` of output `stream` into `dst` (expected
    /// `channels` wide). Returns frames produced (< frames → empty / underrun; rest silenced).
    /// RT-safe.
    std::uint32_t popOutput(std::uint32_t stream, io::AudioBuffer& dst,
                            std::uint32_t frames) noexcept;

    /// The tick (master clock or manual pump): drain each input ring → executor inputs, run
    /// the multi-stream graph, push executor outputs → each output ring. RT-safe (wait-free,
    /// no allocation). Input underrun → silence; output overrun → drop (both counted).
    void process(GraphExecutor& exec, std::uint32_t frames, const io::TimeInfo& time) noexcept;

    /// Per-stream telemetry (M9.1 ring counters): frames the tick couldn't pull from input
    /// `stream` (it hadn't been fed), and frames the tick couldn't push to output `stream`
    /// (the sink wasn't draining).
    [[nodiscard]] std::uint64_t inputUnderruns(std::uint32_t stream) const noexcept;
    [[nodiscard]] std::uint64_t outputOverruns(std::uint32_t stream) const noexcept;

private:
    std::uint32_t numIn_, numOut_, channels_, maxBlock_;
    std::vector<std::unique_ptr<io::RingBuffer<float>>> inRings_;   // [stream*channels + c]
    std::vector<std::unique_ptr<io::RingBuffer<float>>> outRings_;  // [stream*channels + c]
    std::vector<std::vector<float>> inStore_, outStore_;            // planar buffer storage
    std::vector<std::vector<float*>> inPtrs_, outPtrs_;
    std::vector<io::AudioBuffer> inBufs_, outBufs_;                 // executor views
    std::vector<float> zero_;                                       // maxBlock zeros (missing chans)
};

}  // namespace aiudio::graph

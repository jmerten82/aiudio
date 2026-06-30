// aiudio-graph — MasterClockAdapter: makes any AudioBackend the master clock for a
// MultiSourceManager. It is the RenderCallback a backend drives; on each block it pushes
// the device's captured `in` into one manager input stream, pumps the manager (which drains
// all input rings, runs the multi-stream graph, and fills all output rings), then pops one
// manager output stream into the device's `out`. The OTHER sources/sinks (other devices,
// files, Python) feed/drain their own rings off-thread — this device's IOProc is the clock
// that ticks the whole composition (the live-feeding layer for M10, ADR-0014).
//
// RT-safe: pushInput/process/popOutput are all wait-free (no allocation/locks, ADR-0004).
#pragma once

#include <cstdint>

#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/multi_source_manager.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/types.hpp"

namespace aiudio::graph {

class MasterClockAdapter final : public io::RenderCallback {
public:
    /// Drive `manager` (running `exec`) from a backend's clock: this device's `in` feeds
    /// input stream `inStream`, and output stream `outStream` fills this device's `out`.
    MasterClockAdapter(MultiSourceManager& manager, GraphExecutor& exec,
                       std::uint32_t inStream = 0, std::uint32_t outStream = 0) noexcept
        : manager_(manager), exec_(exec), inStream_(inStream), outStream_(outStream) {}

    void process(const io::AudioBuffer& in, io::AudioBuffer& out, std::uint32_t numFrames,
                 const io::TimeInfo& time) noexcept override {
        if (in.numChannels != 0) manager_.pushInput(inStream_, in, numFrames);  // this device → its source
        manager_.process(exec_, numFrames, time);                                // pump the composition
        if (out.numChannels != 0) manager_.popOutput(outStream_, out, numFrames);  // its sink → this device
    }

private:
    MultiSourceManager& manager_;
    GraphExecutor& exec_;
    std::uint32_t inStream_;
    std::uint32_t outStream_;
};

}  // namespace aiudio::graph

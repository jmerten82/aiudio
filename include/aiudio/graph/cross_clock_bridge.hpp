// aiudio-graph — CrossClockBridge (M9.6): the first cross-clock composition — one input
// device + one output device on **separate** physical clocks, kept in sync (ADR-0015 §4,
// realizing ADR-0008/0005). The master device defines the engine clock (per ADR-0005, "one
// duplex callback, swappable clock"); the second output device runs on its OWN crystal and
// drifts, so it cannot be driven by the master's clock. Instead it pulls through a
// drift-compensated `ResamplingSource` (M9.5) whose servo keeps the ring between the two
// clocks bounded.
//
//   master device A (its clock = engine clock)          output device B (its own clock)
//        │ IOProc (master())                                  │ IOProc (output())
//        ▼                                                    ▼
//   push A.capture → manager.process(graph) → pop output ──► outRes_.push()   outRes_.pull() ──► B.out
//                                                            └──────── drift-compensated SPSC ring ───────┘
//
// The cross-clock thread boundary is `outRes_`'s ring: produced on A's IOProc thread, consumed
// on B's IOProc thread (SPSC, ADR-0008 §2). It is **not** N sources — that's Phase C; this is
// the 1-in/1-out stepping stone. All process paths are wait-free (ADR-0004).
//
// Naming caution (carried from docs/76): M9.6 "multi-device" = ONE input + ONE output on
// separate clocks, not N sources.
#pragma once

#include <cstdint>
#include <vector>

#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/multi_source_manager.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/resampling_source.hpp"
#include "aiudio/io/types.hpp"

namespace aiudio::graph {

class CrossClockBridge {
public:
    /// Wire the engine (`mgr` running `exec`) so the master device feeds input stream
    /// `inStream` + drives the graph, and output stream `outStream` is converted from the
    /// engine rate to the output device's rate. `engineRate`/`outDeviceRate` set the nominal
    /// resample ratio (the servo adapts it); `channels`-wide; the cross-clock ring holds
    /// `ringFrames` engine-rate frames; blocks up to `maxBlock`. Allocates (setup-time).
    CrossClockBridge(MultiSourceManager& mgr, GraphExecutor& exec, std::uint32_t inStream,
                     std::uint32_t outStream, double engineRate, double outDeviceRate,
                     std::uint32_t channels, std::uint32_t ringFrames, std::uint32_t maxBlock)
        : mgr_(mgr),
          exec_(exec),
          inStream_(inStream),
          outStream_(outStream),
          channels_(channels == 0 ? 1 : channels),
          maxBlock_(maxBlock == 0 ? 1 : maxBlock) {
        // Output path producer = engine (engineRate), consumer = device B (outDeviceRate).
        outRes_.prepare(channels_, engineRate / outDeviceRate, ringFrames, maxBlock_);
        scratch_.assign(channels_, std::vector<float>(maxBlock_, 0.0f));
        scratchPtrs_.assign(channels_, nullptr);
        for (std::uint32_t c = 0; c < channels_; ++c) scratchPtrs_[c] = scratch_[c].data();
    }

    CrossClockBridge(const CrossClockBridge&) = delete;
    CrossClockBridge& operator=(const CrossClockBridge&) = delete;

    /// The MASTER device's RenderCallback (its clock IS the engine clock). Device A drives it.
    [[nodiscard]] io::RenderCallback& master() noexcept { return master_; }
    /// The OUTPUT device's RenderCallback (its OWN clock). Device B drives it.
    [[nodiscard]] io::RenderCallback& output() noexcept { return output_; }

    // ---- output-path telemetry (atomic; safe from a monitor thread) ----
    [[nodiscard]] double outputRatio() const noexcept { return outRes_.ratio(); }
    [[nodiscard]] std::uint32_t outputFill() const noexcept { return outRes_.fillFrames(); }
    [[nodiscard]] std::uint64_t outputUnderruns() const noexcept { return outRes_.underruns(); }
    [[nodiscard]] std::uint64_t outputOverruns() const noexcept { return outRes_.overruns(); }

private:
    // Master tick: push A's capture → pump the graph → pop the engine output → into the
    // cross-clock ring. Runs on A's IOProc thread.
    struct MasterCb final : io::RenderCallback {
        explicit MasterCb(CrossClockBridge* b) noexcept : b_(b) {}
        void process(const io::AudioBuffer& in, io::AudioBuffer& /*out*/, std::uint32_t frames,
                     const io::TimeInfo& time) noexcept override {
            if (in.numChannels != 0) b_->mgr_.pushInput(b_->inStream_, in, frames);
            b_->mgr_.process(b_->exec_, frames, time);
            io::AudioBuffer s{b_->scratchPtrs_.data(), b_->channels_, frames};
            b_->mgr_.popOutput(b_->outStream_, s, frames);  // engine-rate output
            b_->outRes_.push(s, frames);                    // → cross-clock ring (producer)
        }
        CrossClockBridge* b_;
    };

    // Output tick: pull drift-compensated audio for B's playout. Runs on B's IOProc thread.
    struct OutputCb final : io::RenderCallback {
        explicit OutputCb(CrossClockBridge* b) noexcept : b_(b) {}
        void process(const io::AudioBuffer& /*in*/, io::AudioBuffer& out, std::uint32_t frames,
                     const io::TimeInfo& /*time*/) noexcept override {
            if (out.numChannels != 0) b_->outRes_.pull(out, frames);  // consumer (B's clock)
        }
        CrossClockBridge* b_;
    };

    MultiSourceManager& mgr_;
    GraphExecutor& exec_;
    std::uint32_t inStream_, outStream_, channels_, maxBlock_;
    io::ResamplingSource outRes_;
    std::vector<std::vector<float>> scratch_;  // [channel][maxBlock] engine output staging
    std::vector<float*> scratchPtrs_;
    MasterCb master_{this};
    OutputCb output_{this};
};

}  // namespace aiudio::graph

// aiudio-graph — LiveMultiSource (C1 + C2's off-clock half): compose **N live input sources on
// DIFFERENT clocks** onto one engine timeline, run them through one multi-stream graph, and
// hand the result to a **master output device** whose IOProc is the clock. Realizes the
// aggregate-then-resample model of ADR-0008/0015 for real, arbitrary sources — the piece the
// single-clock MultiSourceManager (M10) and the 1-in/1-out CrossClockBridge (M9.6) did not
// cover.
//
// Model (one master clock; every other clock crosses via a ring + async SRC):
//   source k's capture backend ─drives─► CaptureSourceAdapter[k].process → ResamplingSource[k].push()
//   master output device ─drives─► masterOutput().process → pump():
//        for each source k:  ResamplingSource[k].pull(engineFrames)  → graph input stream k
//        exec.process(inputs[], numIn, outputs[], numOut, frames, time)   (multi-stream, G10)
//        graph output stream `outStream` → the master device's `out`
//
// Each ResamplingSource[k] is SPSC: producer = source k's IOProc thread (push), consumer = the
// master IOProc thread (pull); its drift servo keeps the ring bounded as clock k drifts from the
// master. All process paths are wait-free (ADR-0004); only construction/addSource allocate.
// Compile the executor BEFORE constructing (output-stream count is read here).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/render_callback.hpp"
#include "aiudio/io/resampling_source.hpp"
#include "aiudio/io/types.hpp"

namespace aiudio::graph {

class LiveMultiSource {
public:
    /// `exec` must already be compiled at `engineRate` with `channels` per port; the master
    /// device pulls blocks up to `maxBlock`. Allocates output-side buffers (setup-time).
    LiveMultiSource(GraphExecutor& exec, double engineRate, std::uint32_t channels,
                    std::uint32_t maxBlock)
        : exec_(exec),
          engineRate_(engineRate > 0.0 ? engineRate : 48000.0),
          channels_(channels == 0 ? 1 : channels),
          maxBlock_(maxBlock == 0 ? 1 : maxBlock) {
        masterOut_.owner = this;
        std::uint32_t outStreams = exec.outputStreamCount();
        if (outStreams == 0) outStreams = 1;
        allocStreams(outStore_, outPtrs_, outBufs_, outStreams);
        numOutStreams_ = outStreams;
    }

    LiveMultiSource(const LiveMultiSource&) = delete;
    LiveMultiSource& operator=(const LiveMultiSource&) = delete;

    /// Register an off-clock input source feeding graph input **stream** `streamIndex` (register
    /// streams densely, 0..N-1). Its capture at `sourceRate` is drift-compensated onto the engine
    /// timeline through a `ringFrames`-deep ring. Returns the `RenderCallback` the source's
    /// capture backend must drive (open the backend against it). Setup-time (allocates).
    io::RenderCallback& addSource(std::uint32_t streamIndex, double sourceRate,
                                  std::uint32_t ringFrames) {
        auto s = std::make_unique<Source>();
        s->stream = streamIndex;
        s->res.prepare(channels_, sourceRate / engineRate_, ringFrames, maxBlock_);
        s->adapter.res = &s->res;
        sources_.push_back(std::move(s));
        // Input-stream buffers span 0..max(stream); gaps stay silent.
        std::uint32_t n = 0;
        for (auto& e : sources_)
            if (e->stream + 1 > n) n = e->stream + 1;
        allocStreams(inStore_, inPtrs_, inBufs_, n);
        numInStreams_ = n;
        return sources_.back()->adapter;
    }

    /// The `RenderCallback` the **master output device** drives (its clock pumps everything).
    /// `outStream` = which graph output stream feeds this device.
    io::RenderCallback& masterOutput(std::uint32_t outStream = 0) noexcept {
        masterOut_.outStream = outStream;
        return masterOut_;
    }

    [[nodiscard]] std::uint32_t sourceCount() const noexcept {
        return static_cast<std::uint32_t>(sources_.size());
    }
    // ---- per-source telemetry (atomic; read from any thread) ----
    [[nodiscard]] double sourceRatio(std::uint32_t stream) const noexcept {
        const Source* s = find(stream);
        return s ? s->res.ratio() : 0.0;
    }
    [[nodiscard]] std::uint32_t sourceFill(std::uint32_t stream) const noexcept {
        const Source* s = find(stream);
        return s ? s->res.fillFrames() : 0;
    }
    [[nodiscard]] std::uint64_t sourceUnderruns(std::uint32_t stream) const noexcept {
        const Source* s = find(stream);
        return s ? s->res.underruns() : 0;
    }
    [[nodiscard]] std::uint64_t sourceOverruns(std::uint32_t stream) const noexcept {
        const Source* s = find(stream);
        return s ? s->res.overruns() : 0;
    }

private:
    // Push a source's capture into its ResamplingSource (runs on that source's IOProc thread).
    struct CaptureSourceAdapter final : io::RenderCallback {
        io::ResamplingSource* res = nullptr;
        void process(const io::AudioBuffer& in, io::AudioBuffer& /*out*/, std::uint32_t frames,
                     const io::TimeInfo& /*time*/) noexcept override {
            if (in.numChannels != 0 && res != nullptr) res->push(in, frames);
        }
    };
    struct Source {
        std::uint32_t stream = 0;
        io::ResamplingSource res;
        CaptureSourceAdapter adapter;
    };
    // The master output device's callback: pull every source, run the graph, emit to `out`.
    struct MasterOut final : io::RenderCallback {
        LiveMultiSource* owner = nullptr;
        std::uint32_t outStream = 0;
        void process(const io::AudioBuffer& /*in*/, io::AudioBuffer& out, std::uint32_t frames,
                     const io::TimeInfo& time) noexcept override {
            owner->pump(out, frames, time, outStream);
        }
    };

    void pump(io::AudioBuffer& out, std::uint32_t frames, const io::TimeInfo& time,
              std::uint32_t outStream) noexcept {
        if (frames > maxBlock_) frames = maxBlock_;
        // Pull each off-clock source (drift-compensated to the engine rate) into its stream.
        for (auto& s : sources_) s->res.pull(inBufs_[s->stream], frames);
        exec_.process(inBufs_.data(), numInStreams_, outBufs_.data(), numOutStreams_, frames, time);
        // Emit graph output stream `outStream` to the master device.
        const io::AudioBuffer& src = outBufs_[outStream < numOutStreams_ ? outStream : 0];
        const std::uint32_t ch = out.numChannels < channels_ ? out.numChannels : channels_;
        for (std::uint32_t c = 0; c < ch; ++c) {
            const float* s = src.channel(c);
            float* d = out.channel(c);
            for (std::uint32_t f = 0; f < frames; ++f) d[f] = s[f];
        }
        for (std::uint32_t c = ch; c < out.numChannels; ++c) {  // extra device channels → silence
            float* d = out.channel(c);
            for (std::uint32_t f = 0; f < frames; ++f) d[f] = 0.0f;
        }
    }

    void allocStreams(std::vector<std::vector<float>>& store, std::vector<std::vector<float*>>& ptrs,
                      std::vector<io::AudioBuffer>& bufs, std::uint32_t n) {
        store.assign(n, std::vector<float>(static_cast<std::size_t>(channels_) * maxBlock_, 0.0f));
        ptrs.assign(n, std::vector<float*>(channels_, nullptr));
        bufs.assign(n, io::AudioBuffer{});
        for (std::uint32_t s = 0; s < n; ++s) {
            for (std::uint32_t c = 0; c < channels_; ++c)
                ptrs[s][c] = store[s].data() + static_cast<std::size_t>(c) * maxBlock_;
            bufs[s] = io::AudioBuffer{ptrs[s].data(), channels_, maxBlock_};
        }
    }

    [[nodiscard]] const Source* find(std::uint32_t stream) const noexcept {
        for (const auto& s : sources_)
            if (s->stream == stream) return s.get();
        return nullptr;
    }

    GraphExecutor& exec_;
    double engineRate_;
    std::uint32_t channels_, maxBlock_;
    std::uint32_t numInStreams_ = 0, numOutStreams_ = 0;
    std::vector<std::unique_ptr<Source>> sources_;
    std::vector<std::vector<float>> inStore_, outStore_;
    std::vector<std::vector<float*>> inPtrs_, outPtrs_;
    std::vector<io::AudioBuffer> inBufs_, outBufs_;
    MasterOut masterOut_;
};

}  // namespace aiudio::graph

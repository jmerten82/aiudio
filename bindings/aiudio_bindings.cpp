// aiudio — Python bindings (G6 / M8). nanobind module `_aiudio` exposing the graph
// IR, the executor (with a numpy process() bridge), and the offline file backend,
// so the Python research/ML layer (and later the agent) can build, run, and edit
// graphs. The audio thread / RT core stays in C++ (ADR-0002) — Python drives it.
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aiudio/graph/biquad_node.hpp"
#include "aiudio/graph/cross_clock_bridge.hpp"
#include "aiudio/graph/downmix_node.hpp"
#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/latency_node.hpp"
#include "aiudio/graph/master_clock_adapter.hpp"
#include "aiudio/graph/meter_node.hpp"
#include "aiudio/graph/multi_source_manager.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "aiudio/graph/upmix_node.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/channel_map.hpp"
#include "aiudio/io/drift_compensator.hpp"
#include "aiudio/io/mock_backend.hpp"
#include "aiudio/io/offline_backend.hpp"
#include "aiudio/io/resampler.hpp"
#include "aiudio/io/resampling_source.hpp"
#include "aiudio/io/types.hpp"

// The live device backend (Core Audio) is macOS-only; DeviceBackend is exposed only
// where it exists. Everything else (graph IR, executor, offline) is cross-platform.
#ifdef __APPLE__
#include "aiudio/io/coreaudio_backend.hpp"
#include "aiudio/io/coreaudio_duplex_backend.hpp"
#include "aiudio/io/coreaudio_input_backend.hpp"
#include "aiudio/io/coreaudio_process_tap_backend.hpp"
#endif

namespace nb = nanobind;
using namespace nb::literals;
using namespace aiudio;

namespace {

// Accepts a (channels, frames) float32, C-contiguous CPU array.
using InArray = nb::ndarray<const float, nb::ndim<2>, nb::c_contig, nb::device::cpu>;

// Run one block through the executor: numpy in -> numpy out (both planar:
// shape (channels, frames), row = channel). The graph runs in C++; this is the
// numpy boundary.
nb::ndarray<nb::numpy, float> executorProcess(graph::GraphExecutor& exec, InArray input) {
    const auto channels = static_cast<std::uint32_t>(input.shape(0));
    const auto frames = static_cast<std::uint32_t>(input.shape(1));

    auto* outData = new float[static_cast<std::size_t>(channels) * frames]();
    std::vector<float*> inPtrs(channels), outPtrs(channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        inPtrs[c] = const_cast<float*>(input.data()) + static_cast<std::size_t>(c) * frames;
        outPtrs[c] = outData + static_cast<std::size_t>(c) * frames;
    }
    io::AudioBuffer in{inPtrs.data(), channels, frames};
    io::AudioBuffer out{outPtrs.data(), channels, frames};
    exec.process(in, out, frames, io::TimeInfo{});

    nb::capsule owner(outData, [](void* p) noexcept { delete[] static_cast<float*>(p); });
    return nb::ndarray<nb::numpy, float>(outData, {static_cast<std::size_t>(channels),
                                                   static_cast<std::size_t>(frames)}, owner);
}

// Multi-stream block (G10): a list of (channels, frames) float32 arrays in -> a list of
// (channels, frames) arrays out (one per output stream). Input k feeds SourceNodes bound
// to stream k; output k is written by SinkNodes bound to stream k. The graph runs in C++.
nb::list executorProcessMulti(graph::GraphExecutor& exec, nb::list inputs,
                              std::uint32_t numOutputs) {
    const std::uint32_t numIn = static_cast<std::uint32_t>(inputs.size());
    std::vector<InArray> arrs;
    arrs.reserve(numIn);
    for (std::uint32_t i = 0; i < numIn; ++i) arrs.push_back(nb::cast<InArray>(inputs[i]));

    // All inputs share the block's frame count (taken from the first input).
    const std::uint32_t frames = (numIn > 0) ? static_cast<std::uint32_t>(arrs[0].shape(1)) : 0;
    const std::uint32_t channels = exec.channels();
    if (numOutputs == 0) numOutputs = exec.outputStreamCount();

    std::vector<std::vector<float*>> inPtrs(numIn);
    std::vector<io::AudioBuffer> ins(numIn);
    for (std::uint32_t i = 0; i < numIn; ++i) {
        const auto ch = static_cast<std::uint32_t>(arrs[i].shape(0));
        const auto fr = static_cast<std::uint32_t>(arrs[i].shape(1));
        inPtrs[i].resize(ch);
        for (std::uint32_t c = 0; c < ch; ++c)
            inPtrs[i][c] = const_cast<float*>(arrs[i].data()) + static_cast<std::size_t>(c) * fr;
        ins[i] = io::AudioBuffer{inPtrs[i].data(), ch, fr};
    }

    std::vector<float*> outData(numOutputs, nullptr);
    std::vector<std::vector<float*>> outPtrs(numOutputs);
    std::vector<io::AudioBuffer> outs(numOutputs);
    for (std::uint32_t i = 0; i < numOutputs; ++i) {
        outData[i] = new float[static_cast<std::size_t>(channels) * frames]();
        outPtrs[i].resize(channels);
        for (std::uint32_t c = 0; c < channels; ++c)
            outPtrs[i][c] = outData[i] + static_cast<std::size_t>(c) * frames;
        outs[i] = io::AudioBuffer{outPtrs[i].data(), channels, frames};
    }

    exec.process(ins.data(), numIn, outs.data(), numOutputs, frames, io::TimeInfo{});

    nb::list result;
    for (std::uint32_t i = 0; i < numOutputs; ++i) {
        nb::capsule owner(outData[i], [](void* p) noexcept { delete[] static_cast<float*>(p); });
        result.append(nb::ndarray<nb::numpy, float>(
            outData[i], {static_cast<std::size_t>(channels), static_cast<std::size_t>(frames)},
            owner));
    }
    return result;
}

// Resample one planar (channels, frames) block at the boundary (M9.3). Returns
// (output (channels, produced), input_frames_consumed). The Resampler is stateful, so feed
// successive blocks to the same instance for seamless conversion. `out_cap` bounds how many
// output frames to produce this call.
nb::object resamplerProcess(io::Resampler& rs, InArray input, std::uint32_t outCap) {
    const auto channels = static_cast<std::uint32_t>(input.shape(0));
    const auto inAvail = static_cast<std::uint32_t>(input.shape(1));

    auto* outData = new float[static_cast<std::size_t>(channels) * outCap]();
    std::vector<const float*> inPtrs(channels);
    std::vector<float*> outPtrs(channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        inPtrs[c] = input.data() + static_cast<std::size_t>(c) * inAvail;
        outPtrs[c] = outData + static_cast<std::size_t>(c) * outCap;
    }
    const auto r = rs.process(inPtrs.data(), inAvail, outPtrs.data(), outCap);

    // Compact each channel's `produced` frames to be contiguous (channels, produced).
    if (r.produced != outCap)
        for (std::uint32_t c = 1; c < channels; ++c)
            std::copy_n(outData + static_cast<std::size_t>(c) * outCap, r.produced,
                        outData + static_cast<std::size_t>(c) * r.produced);

    nb::capsule owner(outData, [](void* p) noexcept { delete[] static_cast<float*>(p); });
    nb::ndarray<nb::numpy, float> arr(
        outData, {static_cast<std::size_t>(channels), static_cast<std::size_t>(r.produced)}, owner);
    return nb::make_tuple(arr, r.consumed);
}

// Map a planar (channels, frames) block to `outChannels` at the I/O boundary (M9.2):
// mono↔stereo and N↔M. Returns the mapped (outChannels, frames) array.
nb::ndarray<nb::numpy, float> mapChannelsBlock(InArray src, std::uint32_t outChannels,
                                               io::ChannelMapMode mode) {
    const auto inCh = static_cast<std::uint32_t>(src.shape(0));
    const auto frames = static_cast<std::uint32_t>(src.shape(1));
    std::vector<float*> inPtrs(inCh);
    for (std::uint32_t c = 0; c < inCh; ++c)
        inPtrs[c] = const_cast<float*>(src.data()) + static_cast<std::size_t>(c) * frames;
    io::AudioBuffer in{inPtrs.data(), inCh, frames};

    auto* outData = new float[static_cast<std::size_t>(outChannels) * frames]();
    std::vector<float*> outPtrs(outChannels);
    for (std::uint32_t c = 0; c < outChannels; ++c)
        outPtrs[c] = outData + static_cast<std::size_t>(c) * frames;
    io::AudioBuffer out{outPtrs.data(), outChannels, frames};
    io::mapChannels(in, out, frames, mode);

    nb::capsule owner(outData, [](void* p) noexcept { delete[] static_cast<float*>(p); });
    return nb::ndarray<nb::numpy, float>(
        outData, {static_cast<std::size_t>(outChannels), static_cast<std::size_t>(frames)}, owner);
}

// Engine-side pull from a ResamplingSource (M9.5): produce `frames` of engine-rate audio
// (silence-padded on underrun) as a (channels, frames) numpy array.
nb::ndarray<nb::numpy, float> resamplingSourcePull(io::ResamplingSource& src,
                                                   std::uint32_t frames) {
    const std::uint32_t channels = src.channels();
    auto* outData = new float[static_cast<std::size_t>(channels) * frames]();
    std::vector<float*> outPtrs(channels);
    for (std::uint32_t c = 0; c < channels; ++c)
        outPtrs[c] = outData + static_cast<std::size_t>(c) * frames;
    io::AudioBuffer dst{outPtrs.data(), channels, frames};
    src.pull(dst, frames);

    nb::capsule owner(outData, [](void* p) noexcept { delete[] static_cast<float*>(p); });
    return nb::ndarray<nb::numpy, float>(
        outData, {static_cast<std::size_t>(channels), static_cast<std::size_t>(frames)}, owner);
}

}  // namespace

NB_MODULE(_aiudio, m) {
    m.doc() = "aiudio — graph IR, executor (numpy process), and offline render";

    nb::class_<graph::Graph>(m, "Graph")
        .def(nb::init<>())
        .def("add_source", [](graph::Graph& g, std::uint32_t stream) {
            return g.addNode(std::make_unique<graph::SourceNode>(stream));
        }, "stream"_a = 0, "Add a source bound to input stream `stream` (default 0).")
        .def("add_sink", [](graph::Graph& g, std::uint32_t stream) {
            return g.addNode(std::make_unique<graph::SinkNode>(stream));
        }, "stream"_a = 0, "Add a sink bound to output stream `stream` (default 0).")
        .def("add_gain", [](graph::Graph& g, float gain) {
            return g.addNode(std::make_unique<graph::GainNode>(gain));
        }, "gain"_a)
        .def("add_sum", [](graph::Graph& g, std::uint32_t n) {
            return g.addNode(std::make_unique<graph::SumNode>(n));
        }, "num_inputs"_a = 2)
        .def("add_meter", [](graph::Graph& g) { return g.addNode(std::make_unique<graph::MeterNode>()); })
        .def("add_biquad_lowpass", [](graph::Graph& g, double freq, double q, double sr) {
            auto n = std::make_unique<graph::BiquadNode>();
            n->setLowpass(freq, q, sr);
            return g.addNode(std::move(n));
        }, "freq"_a, "q"_a, "sample_rate"_a)
        .def("add_biquad_highpass", [](graph::Graph& g, double freq, double q, double sr) {
            auto n = std::make_unique<graph::BiquadNode>();
            n->setHighpass(freq, q, sr);
            return g.addNode(std::move(n));
        }, "freq"_a, "q"_a, "sample_rate"_a,
           "Add an RBJ high-pass biquad. set_cutoff/set_q drive it live (re-designs as high-pass).")
        .def("add_biquad_coeffs", [](graph::Graph& g, float b0, float b1, float b2, float a1,
                                     float a2) {
            auto n = std::make_unique<graph::BiquadNode>();
            n->setCoefficients(b0, b1, b2, a1, a2);
            return g.addNode(std::move(n));
        }, "b0"_a, "b1"_a, "b2"_a, "a1"_a, "a2"_a,
           "Add a biquad from raw normalized coefficients (e.g. designed in scipy). a0 is 1.")
        .def("add_downmix", [](graph::Graph& g) {
            return g.addNode(std::make_unique<graph::DownmixNode>());
        }, "Add a down-mix node: N input channels -> 1 (mono average). Changes channel width (G8).")
        .def("add_upmix", [](graph::Graph& g, std::uint32_t channels) {
            return g.addNode(std::make_unique<graph::UpmixNode>(channels));
        }, "channels"_a = 2, "Add an up-mix node: 1 input channel -> `channels` (duplicate). G8.")
        .def("add_latency", [](graph::Graph& g, std::uint32_t frames, std::uint32_t max_channels) {
            return g.addNode(std::make_unique<graph::LatencyNode>(frames, max_channels));
        }, "frames"_a, "max_channels"_a = 2,
           "Add a node that delays by `frames` AND reports that latency — models a "
           "lookahead/FFT/resampler; parallel branches are delay-compensated (G9).")
        .def("connect", [](graph::Graph& g, graph::NodeId s, std::uint32_t sp, graph::NodeId d,
                           std::uint32_t dp) { return g.connect(s, sp, d, dp); },
             "src"_a, "src_port"_a, "dst"_a, "dst_port"_a)
        .def("disconnect", [](graph::Graph& g, graph::NodeId s, std::uint32_t sp, graph::NodeId d,
                              std::uint32_t dp) { return g.disconnect(s, sp, d, dp); },
             "src"_a, "src_port"_a, "dst"_a, "dst_port"_a,
             "Remove the edge src:src_port -> dst:dst_port; returns True if one was removed. "
             "Recompile to apply.")
        .def("remove_node", [](graph::Graph& g, graph::NodeId id) { return g.removeNode(id); },
             "node"_a,
             "Remove a node + all its edges. The id is tombstoned (stays valid-range, node_type "
             "becomes None) so other ids never shift. Returns False if already removed/invalid. "
             "Recompile to apply.")
        .def("validate", [](const graph::Graph& g) {
            const auto r = g.validate();
            return std::make_pair(r.ok, r.error);
        })
        // ---- Introspection (read the IR back; the agent/serialization use this) ----
        .def("nodes", [](const graph::Graph& g) {
            nb::list out;
            for (std::uint32_t id = 0; id < g.nodeCount(); ++id)
                if (const graph::Node* n = g.node(id))
                    out.append(nb::make_tuple(id, n->typeName(), n->numInputs(), n->numOutputs()));
            return out;  // list of (id, type_name, num_inputs, num_outputs) for live nodes
        }, "Live nodes as (id, type_name, num_inputs, num_outputs) tuples (removed ids skipped).")
        .def("edges", [](const graph::Graph& g) {
            nb::list out;
            for (const graph::Edge& e : g.edges())
                out.append(nb::make_tuple(e.src, e.srcPort, e.dst, e.dstPort));
            return out;  // list of (src, src_port, dst, dst_port) tuples
        }, "Edges as (src, src_port, dst, dst_port) tuples.")
        .def("node_type", [](const graph::Graph& g, graph::NodeId id) -> nb::object {
            const graph::Node* n = g.node(id);
            return n ? nb::cast(n->typeName()) : nb::none();
        }, "node"_a, "The node's type name, or None if the id is invalid/removed.")
        .def_prop_ro("live_node_count", [](const graph::Graph& g) { return g.liveNodeCount(); },
                     "Number of live (non-removed) nodes.")
        .def("set_gain", [](graph::Graph& g, graph::NodeId id, float v) {
            if (auto* n = dynamic_cast<graph::GainNode*>(g.node(id))) { n->setGain(v); return true; }
            return false;
        }, "node"_a, "gain"_a)
        .def("meter_mean_square", [](graph::Graph& g, graph::NodeId id) {
            auto* n = dynamic_cast<graph::MeterNode*>(g.node(id));
            return n ? n->meanSquare() : 0.0f;
        }, "node"_a)
        .def_prop_ro("node_count", [](const graph::Graph& g) { return g.nodeCount(); });

    nb::class_<graph::GraphExecutor>(m, "GraphExecutor")
        .def(nb::init<>())
        .def("compile", [](graph::GraphExecutor& e, const graph::Graph& g, std::uint32_t channels,
                           double sampleRate, std::uint32_t maxBlock) {
            return e.compile(g, channels, sampleRate, maxBlock);
        }, "graph"_a, "channels"_a, "sample_rate"_a, "max_block"_a, nb::keep_alive<1, 2>())
        .def("process", &executorProcess, "input"_a,
             "Run one block: (channels, frames) float32 in -> (channels, frames) float32 out.")
        .def("process_multi", &executorProcessMulti, "inputs"_a, "num_outputs"_a = 0,
             "Multi-stream block (G10): list of (channels, frames) float32 arrays in -> list "
             "out (one per output stream). Input k feeds sources on stream k; output k is "
             "written by sinks on stream k. num_outputs=0 -> use the graph's output_streams.")
        .def_prop_ro("channels", [](const graph::GraphExecutor& e) { return e.channels(); },
                     "Compiled channel count per port (0 if not compiled).")
        .def_prop_ro("input_streams", [](const graph::GraphExecutor& e) { return e.inputStreamCount(); },
                     "Number of distinct input streams the compiled graph reads.")
        .def_prop_ro("output_streams", [](const graph::GraphExecutor& e) { return e.outputStreamCount(); },
                     "Number of distinct output streams the compiled graph writes.")
        .def_prop_ro("latency_frames", [](const graph::GraphExecutor& e) { return e.latencyFrames(); },
                     "Total graph latency in frames (parallel branches are delay-compensated; G9).")
        .def_prop_ro("xrun_count", [](const graph::GraphExecutor& e) { return e.xrunCount(); },
                     "Blocks the executor could not fully render (frames > max_block, or not "
                     "compiled) — degraded to silence and counted (M9.1).")
        .def_prop_ro("dropped_commands", [](const graph::GraphExecutor& e) { return e.droppedCommands(); },
                     "Control commands dropped because the lock-free queue was full (M9.1).")
        // ---- Live control plane (RT-safe; lock-free SPSC queue into the audio thread) ----
        // These enqueue a change that the audio thread applies at the top of the next
        // block. Safe to call while a device backend is running — Python never touches
        // the audio thread. Return False if the command queue is momentarily full.
        .def("set_param", [](graph::GraphExecutor& e, graph::NodeId node, std::uint32_t param,
                             float value) { return e.postParam(node, param, value); },
             "node"_a, "param"_a, "value"_a,
             "Queue a control-rate parameter change (applied at the next block).")
        .def("set_gain", [](graph::GraphExecutor& e, graph::NodeId node, float value) {
            return e.postParam(node, graph::GainNode::kGain, value);
        }, "node"_a, "gain"_a, "Live, RT-safe gain change for a GainNode.")
        .def("set_cutoff", [](graph::GraphExecutor& e, graph::NodeId node, float hz) {
            return e.postParam(node, graph::BiquadNode::kCutoffHz, hz);
        }, "node"_a, "hz"_a, "Live, RT-safe cutoff (Hz) change for a BiquadNode.")
        .def("set_q", [](graph::GraphExecutor& e, graph::NodeId node, float q) {
            return e.postParam(node, graph::BiquadNode::kQ, q);
        }, "node"_a, "q"_a, "Live, RT-safe resonance (Q) change for a BiquadNode.")
        .def_prop_ro("render_count", [](const graph::GraphExecutor& e) { return e.renderCount(); },
                     "Blocks the audio thread has processed (telemetry; climbs while audio flows).")
        .def_prop_ro("compiled", [](const graph::GraphExecutor& e) { return e.compiled(); });

    // ---- Multi-source manager (M10): N input sources + M output sinks on one clock ----
    // Producers push numpy into input streams; the pump runs the multi-stream graph; consumers
    // pop numpy from output streams. Each (stream, channel) crosses via its own lock-free ring.
    nb::class_<graph::MultiSourceManager>(m, "MultiSourceManager")
        .def(nb::init<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>(),
             "num_inputs"_a, "num_outputs"_a, "channels"_a, "max_block"_a, "ring_frames"_a)
        .def_prop_ro("num_inputs", [](const graph::MultiSourceManager& mm) { return mm.numInputs(); })
        .def_prop_ro("num_outputs", [](const graph::MultiSourceManager& mm) { return mm.numOutputs(); })
        .def_prop_ro("channels", [](const graph::MultiSourceManager& mm) { return mm.channels(); })
        .def("push_input", [](graph::MultiSourceManager& mm, std::uint32_t stream, InArray src) {
            const auto ch = static_cast<std::uint32_t>(src.shape(0));
            const auto fr = static_cast<std::uint32_t>(src.shape(1));
            std::vector<float*> ptrs(ch);
            for (std::uint32_t c = 0; c < ch; ++c)
                ptrs[c] = const_cast<float*>(src.data()) + static_cast<std::size_t>(c) * fr;
            io::AudioBuffer b{ptrs.data(), ch, fr};
            return mm.pushInput(stream, b, fr);
        }, "stream"_a, "samples"_a,
           "Write a (channels, frames) block into input `stream`; returns frames accepted.")
        .def("pop_output", [](graph::MultiSourceManager& mm, std::uint32_t stream, std::uint32_t frames) {
            const auto ch = mm.channels();
            auto* out = new float[static_cast<std::size_t>(ch) * frames]();
            std::vector<float*> ptrs(ch);
            for (std::uint32_t c = 0; c < ch; ++c) ptrs[c] = out + static_cast<std::size_t>(c) * frames;
            io::AudioBuffer b{ptrs.data(), ch, frames};
            mm.popOutput(stream, b, frames);
            nb::capsule owner(out, [](void* p) noexcept { delete[] static_cast<float*>(p); });
            return nb::ndarray<nb::numpy, float>(out, {static_cast<std::size_t>(ch),
                                                       static_cast<std::size_t>(frames)}, owner);
        }, "stream"_a, "frames"_a, "Read `frames` of output `stream` as a (channels, frames) array.")
        .def("process", [](graph::MultiSourceManager& mm, graph::GraphExecutor& e, std::uint32_t frames) {
            mm.process(e, frames, io::TimeInfo{});
        }, "executor"_a, "frames"_a, nb::call_guard<nb::gil_scoped_release>(),
           "Tick: drain inputs → run the multi-stream graph → fill outputs (pure C++; GIL released).")
        .def("input_underruns", [](const graph::MultiSourceManager& mm, std::uint32_t s) {
            return mm.inputUnderruns(s);
        }, "stream"_a)
        .def("output_overruns", [](const graph::MultiSourceManager& mm, std::uint32_t s) {
            return mm.outputOverruns(s);
        }, "stream"_a);

    // ---- Master clock adapter: make any backend the manager's clock (live-feed M10) ----
    nb::class_<graph::MasterClockAdapter>(m, "MasterClockAdapter")
        .def(nb::init<graph::MultiSourceManager&, graph::GraphExecutor&, std::uint32_t, std::uint32_t>(),
             "manager"_a, "executor"_a, "in_stream"_a = 0, "out_stream"_a = 0,
             nb::keep_alive<1, 2>(), nb::keep_alive<1, 3>(),
             "A RenderCallback that drives `manager` from a backend's clock: this device's "
             "input feeds stream `in_stream`; output stream `out_stream` fills its output.");

    // ---- Cross-clock bridge (M9.6): one input device (master) + one output device on a
    // SEPARATE clock, kept in sync via a drift-compensated output path (ADR-0015 §4). The
    // master device drives master(); the output device drives output() on its own clock. ----
    nb::class_<graph::CrossClockBridge>(m, "CrossClockBridge")
        .def(nb::init<graph::MultiSourceManager&, graph::GraphExecutor&, std::uint32_t,
                      std::uint32_t, double, double, std::uint32_t, std::uint32_t, std::uint32_t>(),
             "manager"_a, "executor"_a, "in_stream"_a, "out_stream"_a, "engine_rate"_a,
             "out_device_rate"_a, "channels"_a, "ring_frames"_a, "max_block"_a,
             nb::keep_alive<1, 2>(), nb::keep_alive<1, 3>(),
             "Engine runs at engine_rate; the output device runs at out_device_rate on its own "
             "clock and pulls through a drift-compensated resampler.")
        .def("attach_master", [](graph::CrossClockBridge& br, io::MockBackend& dev,
                                 std::uint32_t inCh, std::uint32_t outCh, std::uint32_t block) {
            io::StreamConfig c;
            c.inputChannels = inCh;
            c.outputChannels = outCh;
            c.blockSize = block;
            return dev.open(c, &br.master());
        }, "device"_a, "in_channels"_a = 1, "out_channels"_a = 0, "block_size"_a = 64,
           nb::keep_alive<2, 1>(), "Open `device` as the master clock (drives the engine).")
        .def("attach_output", [](graph::CrossClockBridge& br, io::MockBackend& dev,
                                 std::uint32_t inCh, std::uint32_t outCh, std::uint32_t block) {
            io::StreamConfig c;
            c.inputChannels = inCh;
            c.outputChannels = outCh;
            c.blockSize = block;
            return dev.open(c, &br.output());
        }, "device"_a, "in_channels"_a = 0, "out_channels"_a = 1, "block_size"_a = 64,
           nb::keep_alive<2, 1>(), "Open `device` as the off-clock output (pulls drift-compensated).")
        .def_prop_ro("output_ratio", &graph::CrossClockBridge::outputRatio)
        .def_prop_ro("output_fill", &graph::CrossClockBridge::outputFill)
        .def_prop_ro("output_underruns", &graph::CrossClockBridge::outputUnderruns)
        .def_prop_ro("output_overruns", &graph::CrossClockBridge::outputOverruns);

    // ---- Mock backend (M9.4): a deterministic, manually-ticked backend for the live path ----
    nb::class_<io::MockBackend>(m, "MockBackend")
        .def(nb::init<>())
        .def("enumerate", &io::MockBackend::enumerate)
        .def("open", [](io::MockBackend& b, graph::MasterClockAdapter& adapter, std::uint32_t inCh,
                        std::uint32_t outCh, std::uint32_t block) {
            io::StreamConfig c;
            c.inputChannels = inCh;
            c.outputChannels = outCh;
            c.blockSize = block;
            return b.open(c, &adapter);
        }, "adapter"_a, "in_channels"_a = 1, "out_channels"_a = 1, "block_size"_a = 512,
           nb::keep_alive<1, 2>(), "Drive the given MasterClockAdapter from this mock device.")
        .def("set_input_value", &io::MockBackend::setInputValue, "value"_a,
             "The constant value the synthetic capture produces each block.")
        .def("start", &io::MockBackend::start)
        .def("stop", &io::MockBackend::stop)
        .def("tick", &io::MockBackend::tick, "frames"_a,
             "Drive one callback block (the clock tick). No-op if not running / disconnected.")
        .def("inject_disconnect", &io::MockBackend::injectDisconnect, "Simulate a hot-unplug.")
        .def("inject_reconnect", &io::MockBackend::injectReconnect, "Simulate a re-plug.")
        .def("inject_xrun", &io::MockBackend::injectXrun, "frames"_a, "Simulate a device xrun.")
        .def("captured_output", &io::MockBackend::capturedOutput, "channel"_a,
             "First sample of the last block written to output `channel`.")
        .def_prop_ro("running", &io::MockBackend::running)
        .def_prop_ro("disconnected", [](const io::MockBackend& b) { return b.disconnected(); })
        .def_prop_ro("xrun_count", [](const io::MockBackend& b) { return b.xrunCount(); });

    // ---- Boundary resampler (M9.3) — a CONTROL/offline frontend to the same C++ SRC ----
    // Convert a numpy block from one rate to another (ratio = inputRate/outputRate). The
    // instance is stateful: feed consecutive blocks for seamless conversion. `set_ratio`
    // is the hook the drift-compensation loop (M9.5) drives sample-accurately.
    nb::class_<io::Resampler>(m, "Resampler")
        .def("__init__", [](io::Resampler* self, std::uint32_t channels, double ratio) {
            new (self) io::Resampler();
            self->prepare(channels, ratio);
        }, "channels"_a, "ratio"_a, "ratio = input_rate / output_rate (<1 upsamples).")
        .def("process", &resamplerProcess, "block"_a, "out_cap"_a,
             "Resample a planar (channels, frames) block → ((channels, produced), consumed).")
        .def("set_ratio", &io::Resampler::setRatio, "ratio"_a,
             "Change the conversion ratio live (clamped); the drift loop calls this per block.")
        .def("reset", &io::Resampler::reset, "Clear the interpolation state (e.g. after reconnect).")
        .def_prop_ro("ratio", &io::Resampler::ratio)
        .def_prop_ro("channels", &io::Resampler::channels)
        .def_prop_ro("latency_frames", &io::Resampler::latencyFrames,
                     "Kernel group delay in output frames (report to delay compensation).");

    // ---- Off-clock source (M9.5) — ring + resampler + drift servo, as a control frontend ----
    // A producer push()es source-rate audio; the engine pull()s a fixed engine-rate block. The
    // ring-fill servo nudges the resample ratio so a drifting source stays bounded + aligned
    // (ADR-0015). push/pull are wait-free in C++; here they cross the numpy boundary.
    nb::class_<io::ResamplingSource>(m, "ResamplingSource")
        .def("__init__", [](io::ResamplingSource* self, std::uint32_t channels, double nominalRatio,
                            std::uint32_t ringFrames, std::uint32_t maxBlock, double gain,
                            double maxDeviation, double slew) {
            new (self) io::ResamplingSource();
            io::DriftCompensator::Config cfg;
            cfg.gain = gain;
            cfg.maxDeviation = maxDeviation;
            cfg.slew = slew;
            self->prepare(channels, nominalRatio, ringFrames, maxBlock, cfg);
        }, "channels"_a, "nominal_ratio"_a, "ring_frames"_a, "max_block"_a, "gain"_a = 0.05,
           "max_deviation"_a = 0.05, "slew"_a = 0.25,
           "nominal_ratio = source_rate / engine_rate; the servo adapts it from ring fill.")
        .def("push", [](io::ResamplingSource& s, InArray src) {
            const auto ch = static_cast<std::uint32_t>(src.shape(0));
            const auto fr = static_cast<std::uint32_t>(src.shape(1));
            std::vector<float*> ptrs(ch);
            for (std::uint32_t c = 0; c < ch; ++c)
                ptrs[c] = const_cast<float*>(src.data()) + static_cast<std::size_t>(c) * fr;
            io::AudioBuffer b{ptrs.data(), ch, fr};
            return s.push(b, fr);
        }, "block"_a, "Producer: push a (channels, frames) source-rate block; returns frames accepted.")
        .def("pull", &resamplingSourcePull, "frames"_a,
             "Engine: pull `frames` of engine-rate audio (silence-padded on underrun).")
        .def_prop_ro("channels", &io::ResamplingSource::channels)
        .def_prop_ro("ratio", &io::ResamplingSource::ratio, "Current drift-corrected ratio.")
        .def_prop_ro("nominal_ratio", &io::ResamplingSource::nominalRatio)
        .def_prop_ro("fill_frames", &io::ResamplingSource::fillFrames,
                     "Source-rate frames buffered ahead of the engine (telemetry).")
        .def_prop_ro("overruns", &io::ResamplingSource::overruns)
        .def_prop_ro("underruns", &io::ResamplingSource::underruns);

    // ---- Boundary channel mapping (M9.2): mono↔stereo / N↔M at the device↔graph edge ----
    nb::enum_<io::ChannelMapMode>(m, "ChannelMapMode")
        .value("Auto", io::ChannelMapMode::Auto)
        .value("Copy", io::ChannelMapMode::Copy)
        .value("DuplicateMono", io::ChannelMapMode::DuplicateMono)
        .value("DownmixToMono", io::ChannelMapMode::DownmixToMono);
    m.def("map_channels", &mapChannelsBlock, "block"_a, "out_channels"_a,
          "mode"_a = io::ChannelMapMode::Auto,
          "Map a planar (channels, frames) block to out_channels (mono↔stereo / N↔M).");

    // How a backend should respond to an xrun (M9.1). BestEffort: count + keep running
    // (RT-safe degrade). Strict: a backend may stop on the first xrun. Plumbed into open();
    // Strict enforcement on the device backends is still being wired.
    nb::enum_<io::XrunPolicy>(m, "XrunPolicy")
        .value("BestEffort", io::XrunPolicy::BestEffort)
        .value("Strict", io::XrunPolicy::Strict);

    nb::enum_<io::WavFormat>(m, "WavFormat")
        .value("Int16", io::WavFormat::Int16)
        .value("Float32", io::WavFormat::Float32);

    nb::class_<io::OfflineBackend>(m, "OfflineBackend")
        .def(nb::init<std::string, std::string, io::WavFormat>(), "input_wav"_a, "output_wav"_a,
             "output_format"_a = io::WavFormat::Float32)
        .def_prop_ro("input_ok", &io::OfflineBackend::inputOk)
        .def_prop_ro("input_channels", &io::OfflineBackend::inputChannels)
        .def_prop_ro("input_sample_rate", &io::OfflineBackend::inputSampleRate)
        .def_prop_ro("input_frames", &io::OfflineBackend::inputFrames)
        .def("open", [](io::OfflineBackend& b, graph::GraphExecutor& e, std::uint32_t block) {
            io::StreamConfig c;
            c.blockSize = block;
            return b.open(c, &e);
        }, "executor"_a, "block_size"_a = 512, nb::keep_alive<1, 2>())
        .def("start", &io::OfflineBackend::start)
        .def_prop_ro("frames_rendered", &io::OfflineBackend::framesRendered);

#ifdef __APPLE__
    // ---- Live device backend (Core Audio, macOS) — a CONTROL frontend only ----
    // Python opens/starts/stops the stream and observes telemetry; the audio thread
    // (the device IOProc) is pure C++ and never runs Python (ADR-0004). Control-rate
    // edits go through GraphExecutor.set_* (the lock-free queue). start()/stop()/open()
    // release the GIL so the control thread never blocks the engine.
    nb::class_<io::AudioDeviceInfo>(m, "AudioDeviceInfo")
        .def_ro("id", &io::AudioDeviceInfo::id)
        .def_ro("name", &io::AudioDeviceInfo::name)
        .def_ro("input_channels", &io::AudioDeviceInfo::inputChannels)
        .def_ro("output_channels", &io::AudioDeviceInfo::outputChannels)
        .def_ro("sample_rates", &io::AudioDeviceInfo::sampleRates)
        .def_ro("is_default_input", &io::AudioDeviceInfo::isDefaultInput)
        .def_ro("is_default_output", &io::AudioDeviceInfo::isDefaultOutput)
        .def("__repr__", [](const io::AudioDeviceInfo& d) {
            return "<AudioDeviceInfo '" + d.name + "' in=" + std::to_string(d.inputChannels) +
                   " out=" + std::to_string(d.outputChannels) + ">";
        });

    nb::class_<io::CoreAudioBackend>(m, "DeviceBackend")
        .def(nb::init<>())
        .def("enumerate", &io::CoreAudioBackend::enumerate,
             "List the system's audio devices (setup-time; not real-time).")
        .def("open", [](io::CoreAudioBackend& b, graph::GraphExecutor& e, std::uint32_t channels,
                        double sampleRate, std::uint32_t block, const std::string& outputDevice,
                        io::XrunPolicy policy) {
            io::StreamConfig c;
            c.outputChannels = channels;
            c.sampleRate = sampleRate;
            c.blockSize = block;
            c.outputDeviceId = outputDevice;
            c.xrunPolicy = policy;
            return b.open(c, &e);
        }, "executor"_a, "channels"_a = 2, "sample_rate"_a = 48000.0, "block_size"_a = 512,
           "output_device"_a = std::string{}, "xrun_policy"_a = io::XrunPolicy::BestEffort,
           nb::keep_alive<1, 2>(), nb::call_guard<nb::gil_scoped_release>(),
           "Open the output device, routing each RT block to the executor (C++ audio thread).")
        .def("start", &io::CoreAudioBackend::start, nb::call_guard<nb::gil_scoped_release>(),
             "Start the device IOProc (real-time C++ audio thread). Releases the GIL.")
        .def("stop", &io::CoreAudioBackend::stop, nb::call_guard<nb::gil_scoped_release>(),
             "Stop the device IOProc. Releases the GIL.")
        .def("set_disconnect_handler", [](io::CoreAudioBackend& b, nb::callable cb) {
            b.setDisconnectHandler([cb = std::move(cb)]() {
                nb::gil_scoped_acquire gil;  // fired on a HAL thread → take the GIL to call Python
                cb();
            });
        }, "callback"_a,
           "Register a Python callback fired (off the audio thread) when the device is unplugged "
           "/ dies (the HAL device-died listener, M9.4).")
        .def_prop_ro("running", &io::CoreAudioBackend::running)
        .def_prop_ro("latency_frames", &io::CoreAudioBackend::latencyFrames)
        .def_prop_ro("disconnected", [](const io::CoreAudioBackend& b) { return b.disconnected(); },
                     "True once the device was unplugged / died (M9.4 hot-plug listener).")
        .def_prop_ro("xrun_count", [](const io::CoreAudioBackend& b) { return b.xrunCount(); },
                     "Device-side xruns the backend detected (only the device can see these).");

    // ---- Input backend (M3) — capture from an input device (needs mic TCC permission) ----
    nb::class_<io::CoreAudioInputBackend>(m, "InputBackend")
        .def(nb::init<>())
        .def("enumerate", &io::CoreAudioInputBackend::enumerate,
             "List the system's audio devices (setup-time; not real-time).")
        .def("open", [](io::CoreAudioInputBackend& b, graph::GraphExecutor& e, std::uint32_t channels,
                        double sampleRate, std::uint32_t block, const std::string& inputDevice,
                        io::XrunPolicy policy) {
            io::StreamConfig c;
            c.inputChannels = channels;
            c.sampleRate = sampleRate;
            c.blockSize = block;
            c.inputDeviceId = inputDevice;
            c.xrunPolicy = policy;
            return b.open(c, &e);
        }, "executor"_a, "channels"_a = 1, "sample_rate"_a = 48000.0, "block_size"_a = 512,
           "input_device"_a = std::string{}, "xrun_policy"_a = io::XrunPolicy::BestEffort,
           nb::keep_alive<1, 2>(), nb::call_guard<nb::gil_scoped_release>(),
           "Open the input device, routing each captured RT block to the executor (in → graph).")
        .def("start", &io::CoreAudioInputBackend::start, nb::call_guard<nb::gil_scoped_release>())
        .def("stop", &io::CoreAudioInputBackend::stop, nb::call_guard<nb::gil_scoped_release>())
        .def("set_disconnect_handler", [](io::CoreAudioInputBackend& b, nb::callable cb) {
            b.setDisconnectHandler([cb = std::move(cb)]() {
                nb::gil_scoped_acquire gil;
                cb();
            });
        }, "callback"_a, "Python callback fired (off the audio thread) on device disconnect (M9.4).")
        .def_prop_ro("running", &io::CoreAudioInputBackend::running)
        .def_prop_ro("latency_frames", &io::CoreAudioInputBackend::latencyFrames)
        .def_prop_ro("disconnected", [](const io::CoreAudioInputBackend& b) { return b.disconnected(); })
        .def_prop_ro("xrun_count", [](const io::CoreAudioInputBackend& b) { return b.xrunCount(); });

    // ---- Duplex backend (M4) — input+output on ONE clock (mic → graph → speakers) ----
    nb::class_<io::CoreAudioDuplexBackend>(m, "DuplexBackend")
        .def(nb::init<>())
        .def("enumerate", &io::CoreAudioDuplexBackend::enumerate)
        .def("open", [](io::CoreAudioDuplexBackend& b, graph::GraphExecutor& e,
                        std::uint32_t inCh, std::uint32_t outCh, double sampleRate, std::uint32_t block,
                        const std::string& inputDevice, const std::string& outputDevice,
                        io::XrunPolicy policy) {
            io::StreamConfig c;
            c.inputChannels = inCh;
            c.outputChannels = outCh;
            c.sampleRate = sampleRate;
            c.blockSize = block;
            c.inputDeviceId = inputDevice;
            c.outputDeviceId = outputDevice;
            c.xrunPolicy = policy;
            return b.open(c, &e);
        }, "executor"_a, "input_channels"_a = 1, "output_channels"_a = 2, "sample_rate"_a = 48000.0,
           "block_size"_a = 512, "input_device"_a = std::string{}, "output_device"_a = std::string{},
           "xrun_policy"_a = io::XrunPolicy::BestEffort,
           nb::keep_alive<1, 2>(), nb::call_guard<nb::gil_scoped_release>(),
           "Open input+output on one shared clock (an aggregate device if they differ).")
        .def("start", &io::CoreAudioDuplexBackend::start, nb::call_guard<nb::gil_scoped_release>())
        .def("stop", &io::CoreAudioDuplexBackend::stop, nb::call_guard<nb::gil_scoped_release>())
        .def("set_disconnect_handler", [](io::CoreAudioDuplexBackend& b, nb::callable cb) {
            b.setDisconnectHandler([cb = std::move(cb)]() {
                nb::gil_scoped_acquire gil;
                cb();
            });
        }, "callback"_a, "Python callback fired (off the audio thread) on device disconnect (M9.4).")
        .def_prop_ro("running", &io::CoreAudioDuplexBackend::running)
        .def_prop_ro("latency_frames", &io::CoreAudioDuplexBackend::latencyFrames)
        .def_prop_ro("uses_aggregate_device", &io::CoreAudioDuplexBackend::usesAggregateDevice)
        .def_prop_ro("disconnected", [](const io::CoreAudioDuplexBackend& b) { return b.disconnected(); })
        .def_prop_ro("xrun_count", [](const io::CoreAudioDuplexBackend& b) { return b.xrunCount(); });

    // ---- Process-tap backend (M5) — system / per-app output capture (signed + TCC) ----
    nb::class_<io::ProcessInfo>(m, "ProcessInfo")
        .def_ro("pid", &io::ProcessInfo::pid)
        .def_ro("bundle_id", &io::ProcessInfo::bundleId)
        .def("__repr__", [](const io::ProcessInfo& p) {
            return "<ProcessInfo pid=" + std::to_string(p.pid) + " '" + p.bundleId + "'>";
        });

    nb::class_<io::CoreAudioProcessTapBackend>(m, "TapBackend")
        .def(nb::init<>())
        .def_static("list_processes", &io::CoreAudioProcessTapBackend::listProcesses,
                    "List processes Core Audio exposes (pid + bundle id). No permission needed.")
        .def("tap_system_audio", &io::CoreAudioProcessTapBackend::tapSystemAudio,
             "Capture whole-system output (call before open()).")
        .def("tap_process", &io::CoreAudioProcessTapBackend::tapProcess, "pid"_a,
             "Capture a single process's output by PID (call before open()).")
        .def("enumerate", &io::CoreAudioProcessTapBackend::enumerate)
        .def("open", [](io::CoreAudioProcessTapBackend& b, graph::GraphExecutor& e,
                        std::uint32_t channels, double sampleRate, std::uint32_t block,
                        io::XrunPolicy policy) {
            io::StreamConfig c;
            c.inputChannels = channels;
            c.sampleRate = sampleRate;
            c.blockSize = block;
            c.xrunPolicy = policy;
            return b.open(c, &e);
        }, "executor"_a, "channels"_a = 2, "sample_rate"_a = 48000.0, "block_size"_a = 512,
           "xrun_policy"_a = io::XrunPolicy::BestEffort,
           nb::keep_alive<1, 2>(), nb::call_guard<nb::gil_scoped_release>(),
           "Open the tap (needs a signed binary w/ NSAudioCaptureUsageDescription + audio-capture TCC).")
        .def("start", &io::CoreAudioProcessTapBackend::start, nb::call_guard<nb::gil_scoped_release>())
        .def("stop", &io::CoreAudioProcessTapBackend::stop, nb::call_guard<nb::gil_scoped_release>())
        .def("set_disconnect_handler", [](io::CoreAudioProcessTapBackend& b, nb::callable cb) {
            b.setDisconnectHandler([cb = std::move(cb)]() {
                nb::gil_scoped_acquire gil;
                cb();
            });
        }, "callback"_a, "Python callback fired (off the audio thread) on tap/device disconnect (M9.4).")
        .def_prop_ro("running", &io::CoreAudioProcessTapBackend::running)
        .def_prop_ro("latency_frames", &io::CoreAudioProcessTapBackend::latencyFrames)
        .def_prop_ro("disconnected", [](const io::CoreAudioProcessTapBackend& b) { return b.disconnected(); })
        .def_prop_ro("xrun_count", [](const io::CoreAudioProcessTapBackend& b) { return b.xrunCount(); });
#endif  // __APPLE__
}

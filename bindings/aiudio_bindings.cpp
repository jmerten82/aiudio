// aiudio — Python bindings (G6 / M8). nanobind module `_aiudio` exposing the graph
// IR, the executor (with a numpy process() bridge), and the offline file backend,
// so the Python research/ML layer (and later the agent) can build, run, and edit
// graphs. The audio thread / RT core stays in C++ (ADR-0002) — Python drives it.
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "aiudio/graph/biquad_node.hpp"
#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/meter_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "aiudio/io/offline_backend.hpp"
#include "aiudio/io/types.hpp"

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

}  // namespace

NB_MODULE(_aiudio, m) {
    m.doc() = "aiudio — graph IR, executor (numpy process), and offline render";

    nb::class_<graph::Graph>(m, "Graph")
        .def(nb::init<>())
        .def("add_source", [](graph::Graph& g) { return g.addNode(std::make_unique<graph::SourceNode>()); })
        .def("add_sink", [](graph::Graph& g) { return g.addNode(std::make_unique<graph::SinkNode>()); })
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
        .def("connect", [](graph::Graph& g, graph::NodeId s, std::uint32_t sp, graph::NodeId d,
                           std::uint32_t dp) { return g.connect(s, sp, d, dp); },
             "src"_a, "src_port"_a, "dst"_a, "dst_port"_a)
        .def("validate", [](const graph::Graph& g) {
            const auto r = g.validate();
            return std::make_pair(r.ok, r.error);
        })
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
        .def_prop_ro("compiled", [](const graph::GraphExecutor& e) { return e.compiled(); });

    nb::class_<io::OfflineBackend>(m, "OfflineBackend")
        .def(nb::init<std::string, std::string>(), "input_wav"_a, "output_wav"_a)
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
}

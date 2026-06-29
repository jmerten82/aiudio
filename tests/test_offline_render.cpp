// Golden-file test (G4 + M6): render a WAV through a graph offline, bit-exact.
#include <cstdint>
#include <memory>
#include <vector>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/io/offline_backend.hpp"
#include "aiudio/io/wav_file.hpp"
#include "test_framework.hpp"

using namespace aiudio;

AIUDIO_TEST(file_through_gain_graph_is_bit_exact) {
    constexpr std::uint32_t kN = 256;
    std::vector<float> input(kN);
    for (std::uint32_t i = 0; i < kN; ++i) input[i] = static_cast<float>(i % 100) / 100.0f - 0.3f;

    // Write the input (float32, so the round trip is exact).
    {
        io::WavWriter w("aiudio_test_in.wav", 1, 48000.0, io::WavFormat::Float32);
        REQUIRE(w.ok());
        const float* ch[1] = {input.data()};
        w.write(ch, 1, kN);
        w.finalize();
    }

    // Build Source -> Gain(0.5) -> Sink.
    graph::Graph g;
    const auto src = g.addNode(std::make_unique<graph::SourceNode>());
    const auto gain = g.addNode(std::make_unique<graph::GainNode>(0.5f));
    const auto sink = g.addNode(std::make_unique<graph::SinkNode>());
    g.connect(src, 0, gain, 0);
    g.connect(gain, 0, sink, 0);

    // Render the file through the graph offline (the executor IS a RenderCallback).
    io::OfflineBackend ob("aiudio_test_in.wav", "aiudio_test_out.wav", io::WavFormat::Float32);
    REQUIRE(ob.inputOk());
    graph::GraphExecutor exec;
    REQUIRE(exec.compile(g, ob.inputChannels(), ob.inputSampleRate(), /*maxBlock*/ 64));
    io::StreamConfig cfg;
    cfg.blockSize = 64;
    REQUIRE(ob.open(cfg, &exec));
    REQUIRE(ob.start());
    CHECK(ob.framesRendered() == kN);

    // The output must equal input * 0.5, bit-for-bit (float32 path, gain 0.5).
    io::WavReader r("aiudio_test_out.wav");
    REQUIRE(r.ok());
    std::vector<float> out(kN, 0.0f);
    float* och[1] = {out.data()};
    CHECK(r.read(och, 1, kN) == kN);
    bool exact = true;
    for (std::uint32_t i = 0; i < kN; ++i) exact = exact && (out[i] == input[i] * 0.5f);
    CHECK(exact);
}

AIUDIO_TEST_MAIN()

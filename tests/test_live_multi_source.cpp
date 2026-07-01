// Tests for LiveMultiSource: N live input sources on DIFFERENT clocks → one multi-stream graph
// → a master output device. Each source is a MockBackend driving a CaptureSourceAdapter at its
// own tick cadence; a master MockBackend drives the pump. Covers the mix (two distinct sources),
// drift-bounded rings under mismatched clocks, and a multithreaded (TSan) run.
#include "aiudio/graph/live_multi_source.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "aiudio/io/mock_backend.hpp"
#include "aiudio/io/types.hpp"
#include "aiudio/io/wav_file.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;
using aiudio::io::MockBackend;
using aiudio::io::StreamConfig;

namespace {
constexpr double kRe = 48000.0;

// two input streams (0,1) -> SumNode -> sink(0)
std::unique_ptr<Graph> mixGraph() {
    auto g = std::make_unique<Graph>();
    const NodeId a0 = g->addNode(std::make_unique<SourceNode>(0));
    const NodeId a1 = g->addNode(std::make_unique<SourceNode>(1));
    const NodeId mix = g->addNode(std::make_unique<SumNode>(2));
    const NodeId k = g->addNode(std::make_unique<SinkNode>(0));
    g->connect(a0, 0, mix, 0);
    g->connect(a1, 0, mix, 1);
    g->connect(mix, 0, k, 0);
    return g;
}
StreamConfig cfg(std::uint32_t inCh, std::uint32_t outCh, std::uint32_t block) {
    StreamConfig c;
    c.inputChannels = inCh;
    c.outputChannels = outCh;
    c.blockSize = block;
    return c;
}
bool near(float a, float b, float eps = 2e-3f) { return std::fabs(a - b) <= eps; }
}  // namespace

// Two sources on the same rate: their DC values sum at the master output.
AIUDIO_TEST(two_sources_mix_at_master_output) {
    auto g = mixGraph();
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, kRe, 64));
    LiveMultiSource lms(exec, kRe, 1, 64);

    MockBackend srcA, srcB, master;
    REQUIRE(srcA.open(cfg(1, 0, 64), &lms.addSource(0, kRe, 4096)));
    REQUIRE(srcB.open(cfg(1, 0, 64), &lms.addSource(1, kRe, 4096)));
    REQUIRE(master.open(cfg(0, 1, 64), &lms.masterOutput(0)));
    srcA.setInputValue(0.5f);
    srcB.setInputValue(0.2f);
    srcA.start();
    srcB.start();
    master.start();

    for (int i = 0; i < 400; ++i) {  // sources feed, master pumps (same clock here)
        srcA.tick(64);
        srcB.tick(64);
        master.tick(64);
    }
    CHECK(near(master.capturedOutput(0), 0.7f));  // 0.5 + 0.2, mixed onto the engine timeline
    CHECK(lms.sourceUnderruns(0) == 0);
}

// Sources on DIFFERENT clocks: source A 0.2% fast, source B 0.2% slow relative to the master.
// The per-source drift servos keep both rings bounded and the mix stays correct.
AIUDIO_TEST(sources_on_different_clocks_stay_bounded) {
    auto g = mixGraph();
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, kRe, 64));
    const std::uint32_t ring = 8192;
    LiveMultiSource lms(exec, kRe, 1, 64);

    MockBackend srcA, srcB, master;
    REQUIRE(srcA.open(cfg(1, 0, 64), &lms.addSource(0, kRe, ring)));
    REQUIRE(srcB.open(cfg(1, 0, 64), &lms.addSource(1, kRe, ring)));
    REQUIRE(master.open(cfg(0, 1, 64), &lms.masterOutput(0)));
    srcA.setInputValue(0.5f);
    srcB.setInputValue(0.2f);
    srcA.start();
    srcB.start();
    master.start();

    double accA = 0.0, accB = 0.0;
    std::uint32_t maxFillA = 0, maxFillB = 0;
    for (int step = 0; step < 60000; ++step) {
        master.tick(64);                    // the master clock drives the engine
        accA += 64.0 * 1.002;               // source A 0.2% fast
        while (accA >= 64.0) { srcA.tick(64); accA -= 64.0; }
        accB += 64.0 * 0.998;               // source B 0.2% slow
        while (accB >= 64.0) { srcB.tick(64); accB -= 64.0; }
        if (step > 5000) {
            if (lms.sourceFill(0) > maxFillA) maxFillA = lms.sourceFill(0);
            if (lms.sourceFill(1) > maxFillB) maxFillB = lms.sourceFill(1);
        }
    }
    CHECK(lms.sourceOverruns(0) == 0);      // faster source: ring never overflowed
    CHECK(maxFillA < ring && maxFillB < ring);
    CHECK(lms.sourceRatio(0) > 1.0);        // servo sped up consumption of the fast source
    CHECK(lms.sourceRatio(1) < 1.0);        // and slowed the slow source
    CHECK(near(master.capturedOutput(0), 0.7f, 1e-2f));  // mix still ≈ 0.5 + 0.2
}

// Acceptance: two source IOProcs on their own threads + the master IOProc on another —
// race-free (run under TSan). Different clocks; a monitor thread reads telemetry.
AIUDIO_TEST(multithreaded_live_multi_source_is_race_free) {
    auto g = mixGraph();
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, kRe, 64));
    LiveMultiSource lms(exec, kRe, 1, 64);

    MockBackend srcA, srcB, master;
    REQUIRE(srcA.open(cfg(1, 0, 64), &lms.addSource(0, kRe, 16384)));
    REQUIRE(srcB.open(cfg(1, 0, 64), &lms.addSource(1, kRe, 16384)));
    REQUIRE(master.open(cfg(0, 1, 64), &lms.masterOutput(0)));
    srcA.setInputValue(0.5f);
    srcB.setInputValue(0.2f);
    srcA.start();
    srcB.start();
    master.start();

    std::atomic<bool> stop{false};
    std::atomic<long> pumps{0};
    std::atomic<bool> bad{false};

    std::thread ta([&] { while (!stop.load(std::memory_order_relaxed)) srcA.tick(64); });
    std::thread tb([&] { while (!stop.load(std::memory_order_relaxed)) srcB.tick(64); });
    std::thread tm([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            master.tick(64);
            const float v = master.capturedOutput(0);
            if (!std::isfinite(v) || v < -1.5f || v > 1.5f) bad.store(true);
            pumps.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread mon([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            volatile double r = lms.sourceRatio(0);
            volatile std::uint32_t f = lms.sourceFill(1);
            (void)r; (void)f;
        }
    });

    while (pumps.load(std::memory_order_relaxed) < 4000) { /* run */ }
    stop.store(true, std::memory_order_relaxed);
    ta.join();
    tb.join();
    tm.join();
    mon.join();

    CHECK(!bad.load());
    CHECK(pumps.load() > 0);
}

// End-to-end: record the mixed master output to a WAV (the audio thread taps into the
// recorder's ring; the writer thread drains to disk). Read the file back and check the mix.
AIUDIO_TEST(records_master_output_to_wav) {
    auto g = mixGraph();
    GraphExecutor exec;
    REQUIRE(exec.compile(*g, 1, kRe, 64));
    LiveMultiSource lms(exec, kRe, 1, 64);

    MockBackend srcA, srcB, master;
    REQUIRE(srcA.open(cfg(1, 0, 64), &lms.addSource(0, kRe, 4096)));
    REQUIRE(srcB.open(cfg(1, 0, 64), &lms.addSource(1, kRe, 4096)));
    REQUIRE(master.open(cfg(0, 1, 64), &lms.masterOutput(0)));
    srcA.setInputValue(0.5f);
    srcB.setInputValue(0.2f);

    const std::string path = (std::filesystem::temp_directory_path() / "aiudio_lms_rec.wav").string();
    REQUIRE(lms.recordToWav(path, aiudio::io::WavFormat::Float32, 16384));
    CHECK(lms.recording());

    srcA.start();
    srcB.start();
    master.start();
    for (int i = 0; i < 500; ++i) {
        srcA.tick(64);
        srcB.tick(64);
        master.tick(64);  // each pump taps the 0.7 mix into the recorder ring
    }
    lms.stopRecording();  // final drain + finalize
    CHECK(lms.recordedFrames() > 0);
    CHECK(lms.recordDroppedFrames() == 0);

    aiudio::io::WavReader r(path);
    REQUIRE(r.ok());
    CHECK(r.channels() == 1);
    CHECK(r.totalFrames() > 0);
    std::vector<float> c0(static_cast<std::size_t>(r.totalFrames()), 0.0f);
    float* planar[1] = {c0.data()};
    const std::uint32_t got = r.read(planar, 1, static_cast<std::uint32_t>(r.totalFrames()));
    REQUIRE(got > 64);
    CHECK(near(c0[got - 1], 0.7f, 1e-2f));  // steady-state mix 0.5 + 0.2 landed on disk
    std::filesystem::remove(path);
}

AIUDIO_TEST_MAIN()

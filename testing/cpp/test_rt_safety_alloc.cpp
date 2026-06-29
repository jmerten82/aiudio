// RT-safety: prove the audio thread allocates ZERO heap memory (ADR-0004).
//
// The sacred-thread invariant says process() must never allocate. Other tests show
// it produces the *right* numbers and is race-free (TSan); this one proves it touches
// the heap *zero times* — including the full G7 control path (postParam -> drain ->
// Node::setParam, e.g. recomputing biquad coefficients on the audio thread).
//
// Mechanism: replace global operator new/delete with a counter that only ticks while a
// guard flag is set. We arm the guard around the hot path and assert the counter stays
// at zero. A positive self-check first proves the hook actually fires, so a pass can't
// be a false negative.
//
// The hook is compiled out under sanitizer builds (ASan/TSan provide their own
// operator new and would clash); there the test still exercises the hot path for
// crashes/races and reports the hook as skipped.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

#include "aiudio/graph/biquad_node.hpp"
#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/meter_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/source_node.hpp"

using namespace aiudio::graph;

#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
      __has_feature(memory_sanitizer)
#    define AIUDIO_UNDER_SANITIZER 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define AIUDIO_UNDER_SANITIZER 1
#endif

#ifndef AIUDIO_UNDER_SANITIZER
namespace {
// Plain globals (the test is single-threaded): arm `g_guard`, count allocations.
volatile int g_guard = 0;
long g_alloc_count = 0;

inline void* tracked_alloc(std::size_t n) {
    if (g_guard) ++g_alloc_count;
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
}  // namespace

void* operator new(std::size_t n) { return tracked_alloc(n); }
void* operator new[](std::size_t n) { return tracked_alloc(n); }
void* operator new(std::size_t n, std::align_val_t) { return tracked_alloc(n); }
void* operator new[](std::size_t n, std::align_val_t) { return tracked_alloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
#endif  // !AIUDIO_UNDER_SANITIZER

namespace {
constexpr std::uint32_t kFrames = 256;

// Source -> Gain -> Biquad(LP) -> Meter -> Sink: a gain param + a biquad whose
// setParam recomputes coefficients (sin/cos) on the audio thread — the path most at
// risk of a sneaky allocation.
std::unique_ptr<Graph> makeGraph(NodeId& gain, NodeId& biquad) {
    auto g = std::make_unique<Graph>();
    const NodeId s = g->addNode(std::make_unique<SourceNode>());
    gain = g->addNode(std::make_unique<GainNode>(0.5f));
    auto bq = std::make_unique<BiquadNode>(1);
    bq->setLowpass(1000.0, 0.707, 48000.0);
    biquad = g->addNode(std::move(bq));
    const NodeId m = g->addNode(std::make_unique<MeterNode>());
    const NodeId k = g->addNode(std::make_unique<SinkNode>());
    g->connect(s, 0, gain, 0);
    g->connect(gain, 0, biquad, 0);
    g->connect(biquad, 0, m, 0);
    g->connect(m, 0, k, 0);
    return g;
}
}  // namespace

int main() {
    NodeId gain = 0, biquad = 0;
    auto graph = makeGraph(gain, biquad);
    GraphExecutor exec;
    if (!exec.compile(*graph, 1, 48000.0, kFrames)) {
        std::printf("FAIL: compile failed\n");
        return 1;
    }

    float in[kFrames], out[kFrames];
    for (std::uint32_t i = 0; i < kFrames; ++i) in[i] = 0.25f;
    float* ic[1] = {in};
    float* oc[1] = {out};
    AudioBuffer ib{ic, 1, kFrames};
    AudioBuffer ob{oc, 1, kFrames};

    // Warm up outside any guard (forces any first-call lazy init to happen now).
    for (int i = 0; i < 4; ++i) exec.process(ib, ob, kFrames, TimeInfo{});

#ifdef AIUDIO_UNDER_SANITIZER
    // Hook disabled: just exercise the hot path (the sanitizer watches for problems).
    for (int i = 0; i < 1000; ++i) {
        exec.postParam(gain, GainNode::kGain, 0.1f + 0.0001f * static_cast<float>(i % 1000));
        exec.postParam(biquad, BiquadNode::kCutoffHz, 500.0f + static_cast<float>(i % 4000));
        exec.process(ib, ob, kFrames, TimeInfo{});
    }
    std::printf("PASS (alloc hook skipped under sanitizer; hot path exercised)\n");
    return 0;
#else
    // Self-check: the hook must actually fire, or a zero count is meaningless. Call the
    // replaceable allocation function directly — a plain `new int` expression can be
    // *elided* by the optimizer (N3664), but a direct ::operator new call cannot.
    g_alloc_count = 0;
    g_guard = 1;
    void* probe = ::operator new(64);
    g_guard = 0;
    ::operator delete(probe);
    if (g_alloc_count == 0) {
        std::printf("FAIL: allocation hook never fired (test is unreliable)\n");
        return 1;
    }

    // The real measurement: arm the guard around the full control + render hot path.
    g_alloc_count = 0;
    g_guard = 1;
    for (int i = 0; i < 1000; ++i) {
        // Control-thread producer (lock-free push — must not allocate)...
        exec.postParam(gain, GainNode::kGain, 0.1f + 0.0001f * static_cast<float>(i % 1000));
        exec.postParam(biquad, BiquadNode::kCutoffHz, 500.0f + static_cast<float>(i % 4000));
        exec.postParam(biquad, BiquadNode::kQ, 0.707f);
        // ...and the audio thread: drain the queue (setParam, biquad redesign) + render.
        exec.process(ib, ob, kFrames, TimeInfo{});
    }
    g_guard = 0;

    if (g_alloc_count != 0) {
        std::printf("FAIL: %ld heap allocation(s) on the audio/control hot path\n", g_alloc_count);
        return 1;
    }
    std::printf("PASS: 0 allocations across 1000 blocks of postParam + process()\n");
    return 0;
#endif
}

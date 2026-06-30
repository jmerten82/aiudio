// Tests for drift compensation (M9.5): the DriftCompensator servo and the ResamplingSource
// boundary unit. Covers the servo's control law (direction, clamp, convergence under a
// constant clock offset) and — the real acceptance — a long soak where a producer feeds at a
// drifting rate and the engine pulls a fixed block: the ring must stay bounded (no over/
// underruns after warm-up) and the signal stays clean. A scoped allocation hook proves
// push()/pull() never allocate (ADR-0004).
#include "aiudio/io/drift_compensator.hpp"
#include "aiudio/io/resampling_source.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#include "aiudio/io/audio_buffer.hpp"
#include "test_framework.hpp"

using aiudio::io::AudioBuffer;
using aiudio::io::DriftCompensator;
using aiudio::io::ResamplingSource;

// ----- scoped allocation counter (RT-safety proof) ---------------------------------------
namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_track{false};
}  // namespace
void* operator new(std::size_t n) {
    if (g_track.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
DriftCompensator::Config servoCfg(double target) {
    DriftCompensator::Config c;
    c.targetFill = target;
    c.gain = 0.05;
    c.maxDeviation = 0.05;
    c.slew = 0.5;
    return c;
}
}  // namespace

// At exactly the target fill the servo applies no correction.
AIUDIO_TEST(servo_at_target_is_nominal) {
    DriftCompensator d;
    d.prepare(1.0, servoCfg(1000.0));
    const double r = d.update(1000.0);
    CHECK(std::fabs(r - 1.0) < 1e-9);
}

// Over-full → consume faster (ratio up); under-full → consume slower (ratio down).
AIUDIO_TEST(servo_corrects_in_the_right_direction) {
    DriftCompensator a;
    a.prepare(1.0, servoCfg(1000.0));
    a.update(1500.0);  // ring too full
    CHECK(a.ratio() > 1.0);

    DriftCompensator b;
    b.prepare(1.0, servoCfg(1000.0));
    b.update(500.0);  // ring too empty
    CHECK(b.ratio() < 1.0);
}

// The ratio never leaves [nominal·(1-maxDev), nominal·(1+maxDev)], however large the error.
AIUDIO_TEST(servo_is_clamped) {
    DriftCompensator d;
    d.prepare(1.0, servoCfg(1000.0));
    for (int i = 0; i < 100; ++i) d.update(1e9);  // absurdly full
    CHECK(d.ratio() <= 1.05 + 1e-9);
    for (int i = 0; i < 100; ++i) d.update(0.0);  // empty
    CHECK(d.ratio() >= 0.95 - 1e-9);
}

// A constant clock offset (producer 0.2% fast) is tracked: the fill settles in a bounded
// band and the ratio converges to the true rate ratio.
AIUDIO_TEST(servo_converges_under_constant_drift) {
    const double target = 1000.0, out = 64.0, trueRatio = 1.002;
    DriftCompensator d;
    d.prepare(1.0, servoCfg(target));
    double fill = target;
    double minF = fill, maxF = fill;
    for (int i = 0; i < 40000; ++i) {
        const double ratio = d.update(fill);
        fill += trueRatio * out - ratio * out;  // produced − consumed this block
        if (i > 1000) {
            if (fill < minF) minF = fill;
            if (fill > maxF) maxF = fill;
        }
    }
    CHECK(minF > 600.0 && maxF < 1400.0);            // bounded, never near the ring edges
    CHECK(std::fabs(d.ratio() - trueRatio) < 2e-3);  // ratio tracked the true offset
}

// Soak: producer faster than nominal. With drift comp the ring stays bounded — no overruns.
AIUDIO_TEST(source_survives_faster_producer) {
    const std::uint32_t block = 64, ringCap = 4096, ch = 1;
    const double trueRatio = 1.003;  // producer 0.3% fast
    ResamplingSource src;
    src.prepare(ch, /*nominal*/ 1.0, ringCap, block);

    std::vector<float> sbuf(256), dbuf(block, 0.0f);
    double acc = 0.0;
    long long ppos = 0;
    std::uint32_t maxFill = 0;
    bool clean = true;
    for (int step = 0; step < 60000; ++step) {
        acc += block * trueRatio;
        auto nPush = static_cast<std::uint32_t>(acc);
        acc -= nPush;
        if (nPush > sbuf.size()) nPush = static_cast<std::uint32_t>(sbuf.size());
        for (std::uint32_t i = 0; i < nPush; ++i)
            sbuf[i] = std::sin(0.02 * static_cast<double>(ppos + i));
        ppos += nPush;
        const float* sp = sbuf.data();
        AudioBuffer s{const_cast<float**>(&sp), 1, nPush};
        src.push(s, nPush);

        float* dp = dbuf.data();
        AudioBuffer dst{&dp, 1, block};
        src.pull(dst, block);
        if (step > 3000) {
            if (src.fillFrames() > maxFill) maxFill = src.fillFrames();
            for (float v : dbuf)
                if (!std::isfinite(v) || v < -1.5f || v > 1.5f) clean = false;
        }
    }
    CHECK(src.overruns() == 0);    // the ring never overflowed
    CHECK(maxFill < ringCap);      // stayed inside the ring
    CHECK(clean);                  // output stayed finite + in range
}

// Soak: producer slower than nominal. With drift comp the ring stays fed — no underruns.
AIUDIO_TEST(source_survives_slower_producer) {
    const std::uint32_t block = 64, ringCap = 4096, ch = 1;
    const double trueRatio = 0.997;  // producer 0.3% slow
    ResamplingSource src;
    src.prepare(ch, 1.0, ringCap, block);

    std::vector<float> sbuf(256), dbuf(block, 0.0f);
    double acc = 0.0;
    long long ppos = 0;
    for (int step = 0; step < 60000; ++step) {
        acc += block * trueRatio;
        auto nPush = static_cast<std::uint32_t>(acc);
        acc -= nPush;
        for (std::uint32_t i = 0; i < nPush; ++i)
            sbuf[i] = std::sin(0.02 * static_cast<double>(ppos + i));
        ppos += nPush;
        const float* sp = sbuf.data();
        AudioBuffer s{const_cast<float**>(&sp), 1, nPush};
        src.push(s, nPush);
        float* dp = dbuf.data();
        AudioBuffer dst{&dp, 1, block};
        src.pull(dst, block);
    }
    const std::uint64_t under0 = src.underruns();
    // Run a final clean window; after convergence no new underruns should accrue.
    for (int step = 0; step < 5000; ++step) {
        acc += block * trueRatio;
        auto nPush = static_cast<std::uint32_t>(acc);
        acc -= nPush;
        for (std::uint32_t i = 0; i < nPush; ++i)
            sbuf[i] = std::sin(0.02 * static_cast<double>(ppos + i));
        ppos += nPush;
        const float* sp = sbuf.data();
        AudioBuffer s{const_cast<float**>(&sp), 1, nPush};
        src.push(s, nPush);
        float* dp = dbuf.data();
        AudioBuffer dst{&dp, 1, block};
        src.pull(dst, block);
    }
    CHECK(src.underruns() == under0);  // no new starvation after the ring converged
    CHECK(src.ratio() < 1.0);          // servo slowed consumption to match the slow producer
}

// No-drift round trip preserves the signal's energy (a clean sine in → clean sine out).
AIUDIO_TEST(source_roundtrip_preserves_energy) {
    const std::uint32_t block = 64, ch = 1;
    ResamplingSource src;
    src.prepare(ch, 1.0, 2048, block);
    double inSq = 0.0, outSq = 0.0;
    long n = 0;
    std::vector<float> sbuf(block), dbuf(block, 0.0f);
    long long ppos = 0;
    for (int step = 0; step < 4000; ++step) {
        for (std::uint32_t i = 0; i < block; ++i)
            sbuf[i] = 0.5f * std::sin(0.05 * static_cast<double>(ppos + i));
        ppos += block;
        const float* sp = sbuf.data();
        AudioBuffer s{const_cast<float**>(&sp), 1, block};
        src.push(s, block);
        float* dp = dbuf.data();
        AudioBuffer dst{&dp, 1, block};
        src.pull(dst, block);
        if (step > 500) {
            for (std::uint32_t i = 0; i < block; ++i) {
                inSq += static_cast<double>(sbuf[i]) * sbuf[i];
                outSq += static_cast<double>(dbuf[i]) * dbuf[i];
                ++n;
            }
        }
    }
    REQUIRE(n > 0);
    const double inRms = std::sqrt(inSq / n), outRms = std::sqrt(outSq / n);
    CHECK(std::fabs(inRms - outRms) < 0.05);  // energy preserved (no drift → ratio ≈ 1)
}

// push()/pull() never allocate (RT safety, ADR-0004).
AIUDIO_TEST(push_pull_is_allocation_free) {
    const std::uint32_t block = 64, ch = 2;
    ResamplingSource src;
    src.prepare(ch, 1.001, 4096, block);  // allocation happens here (setup)
    std::vector<float> a(block, 0.3f), b(block, -0.4f), oa(block, 0.0f), ob(block, 0.0f);

    g_allocs.store(0);
    g_track.store(true);
    for (int i = 0; i < 2000; ++i) {
        const float* ip[2] = {a.data(), b.data()};
        AudioBuffer s{const_cast<float**>(ip), 2, block};
        src.push(s, block);
        float* op[2] = {oa.data(), ob.data()};
        AudioBuffer dst{op, 2, block};
        src.pull(dst, block);
    }
    g_track.store(false);
    CHECK(g_allocs.load() == 0);
}

// Acceptance: a producer thread push()es while an engine thread pull()s, and a monitor
// thread reads the atomic telemetry — the real cross-thread use. Race-free (run under TSan).
AIUDIO_TEST(resampling_source_is_race_free) {
    const std::uint32_t block = 64, ringCap = 8192, ch = 1;
    ResamplingSource src;
    src.prepare(ch, /*nominal*/ 1.0, ringCap, block);

    std::atomic<bool> stop{false};
    std::atomic<long> pulls{0};
    std::atomic<bool> bad{false};

    std::thread producer([&] {
        std::vector<float> b(block, 0.25f);
        const float* p = b.data();
        AudioBuffer s{const_cast<float**>(&p), 1, block};
        while (!stop.load(std::memory_order_relaxed)) src.push(s, block);
    });
    std::thread engine([&] {
        std::vector<float> b(block, 0.0f);
        while (!stop.load(std::memory_order_relaxed)) {
            float* p = b.data();
            AudioBuffer dst{&p, 1, block};
            src.pull(dst, block);
            for (float v : b)
                if (!std::isfinite(v) || v < -1.5f || v > 1.5f) bad.store(true);
            pulls.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread monitor([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            volatile double r = src.ratio();          // atomic telemetry reads
            volatile std::uint32_t f = src.fillFrames();
            volatile std::uint64_t u = src.underruns();
            (void)r; (void)f; (void)u;
        }
    });

    while (pulls.load(std::memory_order_relaxed) < 3000) { /* run */ }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
    engine.join();
    monitor.join();

    CHECK(!bad.load());
    CHECK(pulls.load() > 0);
}

AIUDIO_TEST_MAIN()

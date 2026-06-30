// Tests for the boundary Resampler (M9.3): a streaming cubic fractional SRC. Covers the
// DSP correctness (DC/linear/sine preserved, length scales by 1/ratio), seamless block
// boundaries (chunked == one-shot), reported latency vs the measured impulse delay, live
// ratio changes (the drift-loop hook), per-channel independence, and — via a scoped global
// allocation hook — that process() never allocates (ADR-0004 RT safety).
#include "aiudio/io/resampler.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include "test_framework.hpp"

using aiudio::io::Resampler;

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
bool close(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Pull all output the resampler can give from `inAvail` input frames (outCap generous).
std::vector<float> resampleAll(Resampler& rs, const std::vector<float>& in, double ratio) {
    rs.prepare(1, ratio);
    const auto inAvail = static_cast<std::uint32_t>(in.size());
    std::vector<float> out(static_cast<std::size_t>(in.size() / ratio) + 8, 0.0f);
    const float* inPtr = in.data();
    float* outPtr = out.data();
    auto r = rs.process(&inPtr, inAvail, &outPtr, static_cast<std::uint32_t>(out.size()));
    out.resize(r.produced);
    return out;
}
}  // namespace

// A constant input resamples to the same constant (Catmull-Rom of a constant is exact).
AIUDIO_TEST(dc_is_preserved) {
    Resampler rs;
    std::vector<float> in(200, 0.7f);
    auto out = resampleAll(rs, in, 44100.0 / 48000.0);  // upsample
    REQUIRE(out.size() > 100);
    for (std::size_t i = 10; i < out.size(); ++i) CHECK(close(out[i], 0.7f));
}

// A linear ramp stays linear: consecutive output diffs equal the ratio (slope is preserved).
AIUDIO_TEST(ramp_stays_linear) {
    Resampler rs;
    const double ratio = 0.5;  // 2x upsample
    std::vector<float> in(128);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i);
    auto out = resampleAll(rs, in, ratio);
    REQUIRE(out.size() > 100);
    for (std::size_t i = 20; i + 1 < out.size(); ++i)
        CHECK(close(out[i + 1] - out[i], static_cast<float>(ratio), 1e-3f));
}

// Output length scales by 1/ratio (within the warm-up/edge slack).
AIUDIO_TEST(length_scales_with_ratio) {
    Resampler rs;
    std::vector<float> in(4800, 0.0f);
    auto up = resampleAll(rs, in, 44100.0 / 48000.0);    // ratio<1 → more output
    auto down = resampleAll(rs, in, 48000.0 / 44100.0);  // ratio>1 → less output
    CHECK(up.size() > in.size());
    CHECK(down.size() < in.size());
    CHECK(std::abs(static_cast<long>(up.size()) - 4800L * 48000 / 44100) < 4);
    CHECK(std::abs(static_cast<long>(down.size()) - 4800L * 44100 / 48000) < 4);
}

// A pure sine keeps its frequency (in Hz): the resampled signal has the same number of
// zero-crossings-per-second, i.e. period scales by 1/ratio in samples but not in time.
AIUDIO_TEST(sine_frequency_is_preserved) {
    Resampler rs;
    const double ratio = 44100.0 / 48000.0;  // 44.1k → 48k
    const int N = 4410;                      // 0.1 s at 44.1k
    const double cyclesPerSample = 0.05;     // ~2205 Hz at 44.1k; 220 cycles total
    std::vector<float> in(N);
    for (int i = 0; i < N; ++i)
        in[i] = std::sin(2.0 * M_PI * cyclesPerSample * i);
    auto out = resampleAll(rs, in, ratio);

    auto crossings = [](const std::vector<float>& v, std::size_t from) {
        int c = 0;
        for (std::size_t i = from + 1; i < v.size(); ++i)
            if ((v[i - 1] <= 0.0f) != (v[i] <= 0.0f)) ++c;
        return c;
    };
    const int inX = crossings(in, 0);
    const int outX = crossings(out, 10);  // skip warm-up
    // Same number of cycles (same time-domain frequency), allowing a couple at the edges.
    CHECK(std::abs(inX - outX) <= 3);
}

// Identity ratio (1.0) is a clean 2-sample passthrough delay — exact, sample for sample.
AIUDIO_TEST(identity_ratio_is_exact_delay) {
    Resampler rs;
    rs.prepare(1, 1.0);
    std::vector<float> in(32);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i + 1);
    std::vector<float> out(32, 0.0f);
    const float* ip = in.data();
    float* op = out.data();
    auto r = rs.process(&ip, 32, &op, 32);
    REQUIRE(r.produced == 32 - 0);  // ratio 1 → as many out as in (minus none; phase aligns)
    // out[n] == in[n-2] for n>=2 (2-sample kernel delay); first two are the zero warm-up.
    CHECK(close(out[0], 0.0f));
    CHECK(close(out[1], 0.0f));
    for (std::size_t n = 2; n < out.size(); ++n) CHECK(close(out[n], in[n - 2]));
}

// Feeding in small chunks gives bit-identical output to one big call (seamless boundaries).
AIUDIO_TEST(chunked_matches_oneshot) {
    const double ratio = 44100.0 / 48000.0;
    std::vector<float> in(1000);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = std::sin(0.03 * static_cast<double>(i));

    Resampler a;
    auto whole = resampleAll(a, in, ratio);

    Resampler b;
    b.prepare(1, ratio);
    std::vector<float> chunked;
    std::uint32_t off = 0;
    const std::uint32_t chunk = 37;  // deliberately not a divisor
    std::vector<float> obuf(4096, 0.0f);
    while (off < in.size()) {
        const std::uint32_t n =
            std::min<std::uint32_t>(chunk, static_cast<std::uint32_t>(in.size()) - off);
        const float* ip = in.data() + off;
        float* op = obuf.data();
        auto r = b.process(&ip, n, &op, static_cast<std::uint32_t>(obuf.size()));
        for (std::uint32_t i = 0; i < r.produced; ++i) chunked.push_back(obuf[i]);
        off += n;
    }
    REQUIRE(chunked.size() >= whole.size());
    for (std::size_t i = 0; i < whole.size(); ++i) CHECK(close(whole[i], chunked[i], 1e-6f));
}

// The reported latency matches the measured impulse group delay (used for PDC, G9).
AIUDIO_TEST(reported_latency_matches_impulse) {
    Resampler rs;
    rs.prepare(1, 1.0);  // identity rate → cleanest measurement
    std::vector<float> in(64, 0.0f);
    in[10] = 1.0f;
    std::vector<float> out(64, 0.0f);
    const float* ip = in.data();
    float* op = out.data();
    rs.process(&ip, 64, &op, 64);
    std::size_t peak = 0;
    for (std::size_t i = 1; i < out.size(); ++i)
        if (out[i] > out[peak]) peak = i;
    const long delay = static_cast<long>(peak) - 10;
    CHECK(std::abs(delay - static_cast<long>(rs.latencyFrames())) <= 1);
}

// Changing the ratio mid-stream (the M9.5 drift hook) keeps output finite + in range.
AIUDIO_TEST(set_ratio_is_live_and_stable) {
    Resampler rs;
    rs.prepare(1, 1.0);
    std::vector<float> obuf(256, 0.0f);
    bool ok = true;
    for (int block = 0; block < 200; ++block) {
        std::vector<float> in(128);
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = std::sin(0.02 * static_cast<double>(block * 128 + static_cast<int>(i)));
        rs.setRatio(1.0 + 0.001 * std::sin(0.1 * block));  // ±0.1% wobble, as drift would
        const float* ip = in.data();
        float* op = obuf.data();
        auto r = rs.process(&ip, 128, &op, static_cast<std::uint32_t>(obuf.size()));
        for (std::uint32_t i = 0; i < r.produced; ++i)
            if (!std::isfinite(obuf[i]) || obuf[i] < -1.5f || obuf[i] > 1.5f) ok = false;
    }
    CHECK(ok);
    CHECK(close(rs.ratio(), 1.0 + 0.001 * std::sin(0.1 * 199), 1e-3f));
}

// Two channels resample independently — no cross-channel bleed.
AIUDIO_TEST(channels_are_independent) {
    Resampler rs;
    rs.prepare(2, 0.5);
    std::vector<float> a(64, 0.4f), b(64, -0.9f);
    std::vector<float> oa(256, 0.0f), ob(256, 0.0f);
    const float* in[2] = {a.data(), b.data()};
    float* out[2] = {oa.data(), ob.data()};
    auto r = rs.process(in, 64, out, 256);
    REQUIRE(r.produced > 100);
    for (std::uint32_t i = 20; i < r.produced; ++i) {
        CHECK(close(oa[i], 0.4f));
        CHECK(close(ob[i], -0.9f));
    }
}

// process() does not allocate (RT safety, ADR-0004) — prove it with the global hook.
AIUDIO_TEST(process_is_allocation_free) {
    Resampler rs;
    rs.prepare(2, 44100.0 / 48000.0);  // allocation happens here (setup), not below
    std::vector<float> a(128, 0.3f), b(128, 0.6f);
    std::vector<float> oa(256, 0.0f), ob(256, 0.0f);
    const float* in[2] = {a.data(), b.data()};

    g_allocs.store(0);
    g_track.store(true);
    for (int i = 0; i < 1000; ++i) {
        rs.setRatio(1.0 + 1e-4 * i);
        float* o0 = oa.data();
        float* o1 = ob.data();
        float* outp[2] = {o0, o1};
        rs.process(in, 128, outp, 256);
    }
    g_track.store(false);
    CHECK(g_allocs.load() == 0);
}

AIUDIO_TEST_MAIN()

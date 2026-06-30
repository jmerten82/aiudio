// Tests for boundary channel mapping (M9.2): mono↔stereo and N↔M at the device↔graph edge.
#include "aiudio/io/channel_map.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include "aiudio/io/audio_buffer.hpp"
#include "test_framework.hpp"

using aiudio::io::AudioBuffer;
using aiudio::io::ChannelMapMode;
using aiudio::io::mapChannels;

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
struct Planar {
    std::vector<std::vector<float>> data;
    std::vector<float*> ptrs;
    explicit Planar(std::uint32_t ch, std::uint32_t frames, float fill = 0.0f) {
        data.assign(ch, std::vector<float>(frames, fill));
        ptrs.resize(ch);
        for (std::uint32_t c = 0; c < ch; ++c) ptrs[c] = data[c].data();
    }
    AudioBuffer buf(std::uint32_t frames) { return AudioBuffer{ptrs.data(), static_cast<std::uint32_t>(ptrs.size()), frames}; }
};
bool close(float a, float b) { return (a > b ? a - b : b - a) < 1e-6f; }
}  // namespace

// mono → stereo: the single channel is duplicated to both outputs (Auto).
AIUDIO_TEST(mono_to_stereo_duplicates) {
    Planar src(1, 8, 0.5f);
    Planar dst(2, 8, -1.0f);
    auto s = src.buf(8), d = dst.buf(8);
    mapChannels(s, d, 8);
    for (std::uint32_t f = 0; f < 8; ++f) {
        CHECK(close(dst.data[0][f], 0.5f));
        CHECK(close(dst.data[1][f], 0.5f));
    }
}

// stereo → mono: the two channels are averaged into the one output (Auto).
AIUDIO_TEST(stereo_to_mono_averages) {
    Planar src(2, 4);
    for (std::uint32_t f = 0; f < 4; ++f) {
        src.data[0][f] = 1.0f;
        src.data[1][f] = 0.4f;
    }
    Planar dst(1, 4);
    auto s = src.buf(4), d = dst.buf(4);
    mapChannels(s, d, 4);
    for (std::uint32_t f = 0; f < 4; ++f) CHECK(close(dst.data[0][f], 0.7f));  // (1.0+0.4)/2
}

// equal widths: a straight copy.
AIUDIO_TEST(equal_width_copies) {
    Planar src(2, 4);
    src.data[0][0] = 0.1f;
    src.data[1][0] = 0.2f;
    Planar dst(2, 4);
    auto s = src.buf(4), d = dst.buf(4);
    mapChannels(s, d, 4);
    CHECK(close(dst.data[0][0], 0.1f));
    CHECK(close(dst.data[1][0], 0.2f));
}

// N→M mismatch (not 1 or →1): copy min, zero-pad the rest. 2→3 keeps two, silences the third.
AIUDIO_TEST(wider_dst_zero_pads) {
    Planar src(2, 4, 0.3f);
    Planar dst(3, 4, 9.0f);
    auto s = src.buf(4), d = dst.buf(4);
    mapChannels(s, d, 4);
    CHECK(close(dst.data[0][0], 0.3f));
    CHECK(close(dst.data[1][0], 0.3f));
    CHECK(close(dst.data[2][0], 0.0f));  // extra dst channel silenced
}

// 3→2 copies the first two, drops the third (Copy under Auto for non-mono cases).
AIUDIO_TEST(narrower_dst_truncates) {
    Planar src(3, 4);
    src.data[0][0] = 0.1f;
    src.data[1][0] = 0.2f;
    src.data[2][0] = 0.9f;
    Planar dst(2, 4);
    auto s = src.buf(4), d = dst.buf(4);
    mapChannels(s, d, 4);
    CHECK(close(dst.data[0][0], 0.1f));
    CHECK(close(dst.data[1][0], 0.2f));  // third source channel dropped
}

// Explicit modes override Auto: force-duplicate and force-downmix.
AIUDIO_TEST(explicit_modes) {
    Planar src(2, 4);
    src.data[0][0] = 0.2f;
    src.data[1][0] = 0.8f;
    Planar dup(2, 4);
    auto s = src.buf(4), dd = dup.buf(4);
    mapChannels(s, dd, 4, ChannelMapMode::DuplicateMono);  // ch0 → both
    CHECK(close(dup.data[0][0], 0.2f));
    CHECK(close(dup.data[1][0], 0.2f));

    Planar mono(1, 4);
    auto dm = mono.buf(4);
    mapChannels(s, dm, 4, ChannelMapMode::DownmixToMono);
    CHECK(close(mono.data[0][0], 0.5f));  // (0.2+0.8)/2
}

// mapChannels does not allocate (RT safety, ADR-0004).
AIUDIO_TEST(map_is_allocation_free) {
    Planar src(1, 128, 0.5f), dst(2, 128);
    auto s = src.buf(128), d = dst.buf(128);
    g_allocs.store(0);
    g_track.store(true);
    for (int i = 0; i < 1000; ++i) mapChannels(s, d, 128);
    g_track.store(false);
    CHECK(g_allocs.load() == 0);
}

AIUDIO_TEST_MAIN()

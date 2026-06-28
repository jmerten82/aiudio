// Tests for interleave/deinterleave and int16<->float32 conversions (M1).
#include "aiudio/io/conversions.hpp"

#include <cmath>
#include <cstdint>

#include "test_framework.hpp"

namespace io = aiudio::io;

AIUDIO_TEST(interleave_deinterleave_roundtrip) {
    constexpr std::uint32_t kCh = 2, kN = 4;
    float left[kN] = {0.0f, 0.1f, 0.2f, 0.3f};
    float right[kN] = {1.0f, 1.1f, 1.2f, 1.3f};
    float* planarIn[kCh] = {left, right};

    float inter[kCh * kN] = {};
    io::interleave(planarIn, inter, kCh, kN);
    // interleaved layout is L0 R0 L1 R1 ...
    CHECK(inter[0] == 0.0f);
    CHECK(inter[1] == 1.0f);
    CHECK(inter[2] == 0.1f);
    CHECK(inter[3] == 1.1f);

    float outL[kN] = {}, outR[kN] = {};
    float* planarOut[kCh] = {outL, outR};
    io::deinterleave(inter, planarOut, kCh, kN);
    bool ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
        ok = ok && (outL[i] == left[i]) && (outR[i] == right[i]);
    }
    CHECK(ok);
}

AIUDIO_TEST(int16_float_roundtrip) {
    const std::int16_t src[5] = {0, 16384, -16384, 32767, -32768};
    float f[5] = {};
    io::int16ToFloat(src, f, 5);
    CHECK(std::fabs(f[0] - 0.0f) < 1e-6f);
    CHECK(std::fabs(f[1] - 0.5f) < 1e-4f);
    CHECK(std::fabs(f[2] - (-0.5f)) < 1e-4f);
    CHECK(f[3] > 0.99f);
    CHECK(f[4] <= -1.0f);  // -32768/32768 == -1.0 exactly

    std::int16_t back[5] = {};
    io::floatToInt16(f, back, 5);
    CHECK(back[0] == 0);
    CHECK(std::abs(back[1] - 16383) <= 1);  // 0.5*32767 ≈ 16383
}

AIUDIO_TEST(float_to_int16_clamps) {
    const float src[4] = {2.0f, -2.0f, 1.5f, -1.5f};
    std::int16_t out[4] = {};
    io::floatToInt16(src, out, 4);
    CHECK(out[0] == 32767);   // clamped from +2.0
    CHECK(out[1] == -32767);  // clamped from -2.0 (1.0 * 32767)
    CHECK(out[2] == 32767);
    CHECK(out[3] == -32767);
}

AIUDIO_TEST_MAIN()

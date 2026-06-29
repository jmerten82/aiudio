// Tests for the WAV reader/writer (M6).
#include "aiudio/io/wav_file.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include "test_framework.hpp"

using namespace aiudio::io;

namespace {
constexpr std::uint32_t kN = 200;
std::vector<float> ramp() {
    std::vector<float> v(kN);
    for (std::uint32_t i = 0; i < kN; ++i) v[i] = static_cast<float>(i) / kN - 0.5f;  // -0.5..0.5
    return v;
}
}  // namespace

AIUDIO_TEST(float32_roundtrip_is_exact) {
    const std::vector<float> data = ramp();
    const float* in[1] = {data.data()};
    {
        WavWriter w("aiudio_test_f32.wav", 1, 48000.0, WavFormat::Float32);
        REQUIRE(w.ok());
        w.write(in, 1, kN);
        w.finalize();
    }
    WavReader r("aiudio_test_f32.wav");
    REQUIRE(r.ok());
    CHECK(r.channels() == 1);
    CHECK(r.totalFrames() == kN);

    std::vector<float> back(kN, 0.0f);
    float* out[1] = {back.data()};
    CHECK(r.read(out, 1, kN) == kN);
    bool exact = true;
    for (std::uint32_t i = 0; i < kN; ++i) exact = exact && (back[i] == data[i]);
    CHECK(exact);  // float32 round-trips bit-for-bit
}

AIUDIO_TEST(int16_roundtrip_within_one_lsb) {
    const std::vector<float> data = ramp();
    const float* in[1] = {data.data()};
    {
        WavWriter w("aiudio_test_i16.wav", 1, 48000.0, WavFormat::Int16);
        REQUIRE(w.ok());
        w.write(in, 1, kN);
        w.finalize();
    }
    WavReader r("aiudio_test_i16.wav");
    REQUIRE(r.ok());
    std::vector<float> back(kN, 0.0f);
    float* out[1] = {back.data()};
    r.read(out, 1, kN);
    bool within = true;
    for (std::uint32_t i = 0; i < kN; ++i) within = within && (std::fabs(back[i] - data[i]) < 2.0f / 32768.0f);  // int16 is lossy (~1 LSB)
    CHECK(within);
}

AIUDIO_TEST_MAIN()

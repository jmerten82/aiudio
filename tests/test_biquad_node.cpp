// Tests for BiquadNode (G4): RBJ low-/high-pass DC behaviour.
#include "aiudio/graph/biquad_node.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
// Feed a constant 1.0 through the biquad and return the settled (last) sample.
float settledDcOutput(BiquadNode& bq) {
    constexpr std::uint32_t kN = 4096;
    std::vector<float> in(kN, 1.0f), out(kN, 0.0f);
    float* ich[1] = {in.data()};
    float* och[1] = {out.data()};
    AudioBuffer ib{ich, 1, kN};
    AudioBuffer ob{och, 1, kN};
    bq.process(&ib, &ob, kN, TimeInfo{});
    return out[kN - 1];
}
}  // namespace

AIUDIO_TEST(lowpass_passes_dc) {
    BiquadNode bq(1);
    bq.setLowpass(1000.0, 0.707, 48000.0);
    bq.prepare(48000.0, 4096);
    CHECK(std::fabs(settledDcOutput(bq) - 1.0f) < 1e-3f);  // lowpass DC gain ≈ 1
}

AIUDIO_TEST(highpass_blocks_dc) {
    BiquadNode bq(1);
    bq.setHighpass(1000.0, 0.707, 48000.0);
    bq.prepare(48000.0, 4096);
    CHECK(std::fabs(settledDcOutput(bq)) < 1e-3f);  // highpass blocks DC → ≈ 0
}

AIUDIO_TEST_MAIN()

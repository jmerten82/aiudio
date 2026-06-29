// Test for MeterNode (G3): passthrough + level measurement, offline.
#include "aiudio/graph/meter_node.hpp"

#include <cmath>
#include <cstdint>

#include "test_framework.hpp"

using namespace aiudio::graph;

AIUDIO_TEST(meter_passes_through_and_measures) {
    constexpr std::uint32_t kN = 64;
    float inData[kN], outData[kN];
    for (std::uint32_t i = 0; i < kN; ++i) { inData[i] = 0.5f; outData[i] = -1.0f; }
    float* inCh[1] = {inData};
    float* outCh[1] = {outData};
    AudioBuffer in{inCh, 1, kN};
    AudioBuffer out{outCh, 1, kN};

    MeterNode meter;
    meter.prepare(48000.0, kN);
    meter.process(&in, &out, kN, TimeInfo{});

    bool passthrough = true;
    for (std::uint32_t i = 0; i < kN; ++i) passthrough = passthrough && (outData[i] == 0.5f);
    CHECK(passthrough);
    CHECK(std::fabs(meter.meanSquare() - 0.25f) < 1e-6f);  // 0.5^2
    CHECK(meter.calls() == 1);
}

AIUDIO_TEST_MAIN()

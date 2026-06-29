// Tests for the trivial nodes (GainNode, SumNode) in isolation (G1).
#include <cmath>
#include <cstdint>

#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;

namespace {
constexpr std::uint32_t kN = 64;
}  // namespace

AIUDIO_TEST(gain_node_scales_samples) {
    float inData[kN], outData[kN];
    float* inCh[1] = {inData};
    float* outCh[1] = {outData};
    for (std::uint32_t i = 0; i < kN; ++i) inData[i] = 1.0f;
    AudioBuffer in{inCh, 1, kN};
    AudioBuffer out{outCh, 1, kN};

    GainNode gain(0.5f);
    gain.prepare(48000.0, kN);
    gain.process(&in, &out, kN, TimeInfo{});

    bool ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) ok = ok && (std::fabs(outData[i] - 0.5f) < 1e-6f);
    CHECK(ok);
    CHECK(gain.numInputs() == 1);
    CHECK(gain.numOutputs() == 1);
}

AIUDIO_TEST(sum_node_mixes_inputs) {
    float a[kN], b[kN], outData[kN];
    float* aCh[1] = {a};
    float* bCh[1] = {b};
    float* outCh[1] = {outData};
    for (std::uint32_t i = 0; i < kN; ++i) { a[i] = 1.0f; b[i] = 2.0f; }
    AudioBuffer ins[2] = {AudioBuffer{aCh, 1, kN}, AudioBuffer{bCh, 1, kN}};
    AudioBuffer out{outCh, 1, kN};

    SumNode sum(2);
    sum.prepare(48000.0, kN);
    sum.process(ins, &out, kN, TimeInfo{});

    bool ok = true;
    for (std::uint32_t i = 0; i < kN; ++i) ok = ok && (std::fabs(outData[i] - 3.0f) < 1e-6f);
    CHECK(ok);
    CHECK(sum.numInputs() == 2);
    CHECK(sum.numOutputs() == 1);
}

AIUDIO_TEST_MAIN()

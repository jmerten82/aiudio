// Tests for ParametricEqNode: equivalence to a single BiquadNode (1 band), band cascading,
// per-band live param routing, and empty-passthrough.
#include "aiudio/graph/parametric_eq_node.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include "aiudio/graph/biquad_node.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;
using aiudio::io::AudioBuffer;
using aiudio::io::TimeInfo;

namespace {
constexpr double SR = 48000.0;

// Feed constant 1.0, return the settled (last) output sample of a 1-in/1-out node.
template <class NodeT>
float settledDc(NodeT& node, std::uint32_t n = 4096) {
    node.prepare(SR, n);
    std::vector<float> in(n, 1.0f), out(n, 0.0f);
    float* ic[1] = {in.data()};
    float* oc[1] = {out.data()};
    AudioBuffer ib{ic, 1, n};
    AudioBuffer ob{oc, 1, n};
    node.process(&ib, &ob, n, TimeInfo{});
    return out[n - 1];
}
}  // namespace

// A 1-band EQ is identical to the equivalent standalone BiquadNode (same design + cascade-of-1).
AIUDIO_TEST(single_band_equals_biquad) {
    ParametricEqNode eq({{BiquadNode::Type::Peaking, 1500.0, 1.0, 6.0}}, SR, 1);
    BiquadNode bq(1);
    bq.setPeaking(1500.0, 1.0, 6.0, SR);
    eq.prepare(SR, 256);
    bq.prepare(SR, 256);

    std::vector<float> in(256), eqOut(256, 0.0f), bqOut(256, 0.0f);
    for (std::uint32_t f = 0; f < 256; ++f) in[f] = std::sin(0.07f * f) + 0.3f * std::sin(0.31f * f);
    float* ic[1] = {in.data()};
    float* eo[1] = {eqOut.data()};
    float* bo[1] = {bqOut.data()};
    AudioBuffer ib{ic, 1, 256};
    AudioBuffer eqb{eo, 1, 256};
    AudioBuffer bqb{bo, 1, 256};
    eq.process(&ib, &eqb, 256, TimeInfo{});
    bq.process(&ib, &bqb, 256, TimeInfo{});
    bool identical = true;
    for (std::uint32_t f = 0; f < 256; ++f)
        if (std::fabs(eqOut[f] - bqOut[f]) > 1e-6f) identical = false;
    CHECK(identical);
}

// A low-shelf +6 dB band boosts DC ~2x even cascaded with flat (0 dB) peaking/high-shelf bands.
AIUDIO_TEST(cascade_low_shelf_boosts_dc) {
    ParametricEqNode eq(
        {
            {BiquadNode::Type::LowShelf, 200.0, 0.707, 6.0},
            {BiquadNode::Type::Peaking, 2000.0, 1.0, 0.0},   // flat
            {BiquadNode::Type::HighShelf, 8000.0, 0.707, 0.0},  // flat
        },
        SR, 1);
    CHECK(eq.numBands() == 3);
    CHECK(std::fabs(settledDc(eq) - 1.995f) < 0.05f);  // +6 dB at DC, others flat
}

// Per-band live control: index = band*3 + {0:freq,1:Q,2:gain}. Lift band 0's gain → DC rises.
AIUDIO_TEST(per_band_param_routing) {
    ParametricEqNode eq({{BiquadNode::Type::LowShelf, 200.0, 0.707, 0.0}}, SR, 1);
    eq.prepare(SR, 4096);
    // band 0, param 2 (gain dB) → +6 dB
    eq.setParam(0 * ParametricEqNode::kParamsPerBand + 2, 6.0f);
    std::vector<float> in(4096, 1.0f), out(4096, 0.0f);
    float* ic[1] = {in.data()};
    float* oc[1] = {out.data()};
    AudioBuffer ib{ic, 1, 4096};
    AudioBuffer ob{oc, 1, 4096};
    eq.process(&ib, &ob, 4096, TimeInfo{});
    CHECK(std::fabs(out[4095] - 1.995f) < 0.05f);
}

AIUDIO_TEST(empty_eq_is_passthrough) {
    ParametricEqNode eq({}, SR, 1);
    CHECK(eq.numBands() == 0);
    CHECK(std::fabs(settledDc(eq) - 1.0f) < 1e-6f);
}

AIUDIO_TEST_MAIN()

// Tests for the dynamics nodes: CompressorNode (gain reduction above threshold + lookahead
// latency reporting) and GateNode (attenuation below threshold).
#include <cmath>
#include <cstdint>
#include <vector>

#include "aiudio/graph/compressor_node.hpp"
#include "aiudio/graph/gate_node.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;
using aiudio::io::AudioBuffer;
using aiudio::io::TimeInfo;

namespace {
constexpr double SR = 48000.0;

// Drive a 1-in/1-out dynamics node with a constant level for `blocks` blocks; return the
// settled output level (last sample) for a constant input `level`.
template <class NodeT>
float settledLevel(NodeT& node, float level, int blocks = 200, std::uint32_t n = 256) {
    node.prepare(SR, n);
    std::vector<float> in(n, level), out(n, 0.0f);
    float* ic[1] = {in.data()};
    float* oc[1] = {out.data()};
    AudioBuffer ib{ic, 1, n};
    AudioBuffer ob{oc, 1, n};
    for (int i = 0; i < blocks; ++i) node.process(&ib, &ob, n, TimeInfo{});
    return std::fabs(out[n - 1]);
}
}  // namespace

// Below threshold: no reduction (unity). Above threshold: output is attenuated, and the
// attenuation is bounded by the ratio (a loud input is reduced toward the threshold).
AIUDIO_TEST(compressor_reduces_above_threshold) {
    CompressorNode quiet(-12.0f, 4.0f, 1.0f, 50.0f, 0, 1);
    const float lo = settledLevel(quiet, 0.1f);   // -20 dBFS, below -12 dB threshold
    CHECK(std::fabs(lo - 0.1f) < 1e-3f);           // passes unchanged

    CompressorNode loud(-12.0f, 4.0f, 1.0f, 50.0f, 0, 1);
    const float hi = settledLevel(loud, 1.0f);     // 0 dBFS, well above threshold
    CHECK(hi < 0.95f);                              // compressed down
    // 0 dBFS, thresh -12, ratio 4 → output ≈ -12 + 12/4 = -9 dBFS ≈ 0.355 (no makeup)
    CHECK(hi > 0.2f && hi < 0.5f);
}

AIUDIO_TEST(compressor_lookahead_reports_latency) {
    CompressorNode noLA(-12.0f, 4.0f, 1.0f, 50.0f, /*lookahead*/ 0, 1);
    CHECK(noLA.latencyFrames() == 0);
    CompressorNode la(-12.0f, 4.0f, 1.0f, 50.0f, /*lookahead*/ 32, 1);
    CHECK(la.latencyFrames() == 32);  // reported via the G9 contract for PDC
}

// Below threshold the gate closes toward the range floor; above, it opens to unity.
AIUDIO_TEST(gate_attenuates_below_threshold) {
    GateNode g(-30.0f, 1.0f, 50.0f, -80.0f);
    const float quiet = settledLevel(g, 0.005f);   // ~-46 dBFS, below -30 → gated
    CHECK(quiet < 0.005f * 0.1f);                   // strongly attenuated toward the floor

    GateNode g2(-30.0f, 1.0f, 50.0f, -80.0f);
    const float loud = settledLevel(g2, 0.5f);      // ~-6 dBFS, above threshold → open
    CHECK(std::fabs(loud - 0.5f) < 1e-2f);          // passes ~unchanged
}

AIUDIO_TEST_MAIN()

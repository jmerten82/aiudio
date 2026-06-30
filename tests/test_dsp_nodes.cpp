// Tests for the Tier-1 node library (generators, shaping, routing, delay, utilities).
// Each node is driven directly through prepare()+process() with hand-sized planar buffers.
#include <cmath>
#include <cstdint>
#include <vector>

#include "aiudio/graph/channel_matrix_node.hpp"
#include "aiudio/graph/dc_blocker_node.hpp"
#include "aiudio/graph/delay_node.hpp"
#include "aiudio/graph/mixer_node.hpp"
#include "aiudio/graph/noise_node.hpp"
#include "aiudio/graph/oscillator_node.hpp"
#include "aiudio/graph/pan_node.hpp"
#include "aiudio/graph/stereo_width_node.hpp"
#include "aiudio/graph/waveshaper_node.hpp"
#include "aiudio/io/audio_buffer.hpp"
#include "test_framework.hpp"

using namespace aiudio::graph;
using aiudio::io::AudioBuffer;
using aiudio::io::TimeInfo;

namespace {
constexpr double SR = 48000.0;
bool close(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// One planar channel buffer + AudioBuffer view helper.
struct Buf {
    std::vector<std::vector<float>> data;
    std::vector<float*> ptrs;
    Buf(std::uint32_t ch, std::uint32_t frames, float fill = 0.0f) {
        data.assign(ch, std::vector<float>(frames, fill));
        ptrs.resize(ch);
        for (std::uint32_t c = 0; c < ch; ++c) ptrs[c] = data[c].data();
    }
    AudioBuffer view(std::uint32_t frames) {
        return AudioBuffer{ptrs.data(), static_cast<std::uint32_t>(ptrs.size()), frames};
    }
};
}  // namespace

AIUDIO_TEST(waveshaper_tanh_saturates_and_dry_passes) {
    WaveshaperNode ws(WaveshaperNode::Shape::Tanh, /*drive*/ 1.0f, /*mix*/ 1.0f);
    ws.prepare(SR, 64);
    Buf in(1, 64, 10.0f), out(1, 64);
    AudioBuffer ib = in.view(64), ob = out.view(64);
    ws.process(&ib, &ob, 64, TimeInfo{});
    CHECK(out.data[0][63] > 0.99f && out.data[0][63] <= 1.0f);  // tanh(10·1) ≈ 1

    WaveshaperNode dry(WaveshaperNode::Shape::Tanh, 5.0f, /*mix*/ 0.0f);
    dry.prepare(SR, 64);
    Buf in2(1, 64, 0.3f), out2(1, 64);
    AudioBuffer ib2 = in2.view(64), ob2 = out2.view(64);
    dry.process(&ib2, &ob2, 64, TimeInfo{});
    CHECK(close(out2.data[0][63], 0.3f));  // mix 0 → dry passthrough
}

AIUDIO_TEST(oscillator_sine_is_bounded_and_nonzero) {
    OscillatorNode osc(OscillatorNode::Waveform::Sine, 1000.0, 0.5f);
    osc.prepare(SR, 256);
    Buf out(1, 256);
    AudioBuffer ob = out.view(256);
    osc.process(nullptr, &ob, 256, TimeInfo{});
    float maxAbs = 0.0f;
    int signChanges = 0;
    for (std::uint32_t f = 0; f < 256; ++f) {
        maxAbs = std::fmax(maxAbs, std::fabs(out.data[0][f]));
        if (f > 0 && (out.data[0][f - 1] < 0.0f) != (out.data[0][f] < 0.0f)) ++signChanges;
    }
    CHECK(maxAbs > 0.4f && maxAbs <= 0.5001f);  // amplitude 0.5
    CHECK(signChanges >= 8);                    // genuinely oscillating (~5 cycles → ~10 crossings)
}

AIUDIO_TEST(noise_is_bounded_and_nonzero) {
    NoiseNode n(NoiseNode::Color::White, 0.5f, 2);
    n.prepare(SR, 512);
    Buf out(2, 512);
    AudioBuffer ob = out.view(512);
    n.process(nullptr, &ob, 512, TimeInfo{});
    float maxAbs = 0.0f;
    bool nonzero = false, decorrelated = false;
    for (std::uint32_t f = 0; f < 512; ++f) {
        maxAbs = std::fmax(maxAbs, std::fabs(out.data[0][f]));
        if (out.data[0][f] != 0.0f) nonzero = true;
        if (out.data[0][f] != out.data[1][f]) decorrelated = true;
    }
    CHECK(nonzero && decorrelated);
    CHECK(maxAbs <= 0.5001f);
}

AIUDIO_TEST(dc_blocker_removes_dc) {
    DcBlockerNode dc(20.0, 1);
    dc.prepare(SR, 4096);
    Buf in(1, 4096, 1.0f), out(1, 4096);
    AudioBuffer ib = in.view(4096), ob = out.view(4096);
    dc.process(&ib, &ob, 4096, TimeInfo{});
    CHECK(std::fabs(out.data[0][4095]) < 1e-2f);  // settled DC → ~0
}

AIUDIO_TEST(stereo_width_zero_is_mono) {
    StereoWidthNode w(0.0f);  // width 0 → L==R
    w.prepare(SR, 64);
    Buf in(2, 64), out(2, 64);
    for (std::uint32_t f = 0; f < 64; ++f) { in.data[0][f] = 1.0f; in.data[1][f] = -0.5f; }
    AudioBuffer ib = in.view(64), ob = out.view(64);
    w.process(&ib, &ob, 64, TimeInfo{});
    CHECK(close(out.data[0][63], out.data[1][63]));         // collapsed to mono
    CHECK(close(out.data[0][63], 0.25f));                   // mid = (1 + -0.5)/2
}

AIUDIO_TEST(pan_center_is_equal_power) {
    PanNode p(0.0f);  // centre
    p.prepare(SR, 64);
    Buf in(1, 64, 1.0f), out(2, 64);  // mono in, stereo out
    AudioBuffer ib = in.view(64), ob = out.view(64);
    p.process(&ib, &ob, 64, TimeInfo{});
    CHECK(close(out.data[0][63], 0.70710677f, 1e-3f));  // cos(45°)
    CHECK(close(out.data[1][63], 0.70710677f, 1e-3f));

    PanNode left(-1.0f);
    left.prepare(SR, 64);
    Buf in2(1, 64, 1.0f), out2(2, 64);
    AudioBuffer ib2 = in2.view(64), ob2 = out2.view(64);
    left.process(&ib2, &ob2, 64, TimeInfo{});
    CHECK(close(out2.data[0][63], 1.0f, 1e-3f));   // hard left → all in L
    CHECK(close(out2.data[1][63], 0.0f, 1e-3f));
}

AIUDIO_TEST(mixer_weighted_sum) {
    MixerNode m(2, 1.0f);
    m.prepare(SR, 64);     // prepare snaps smoothers to the ctor default; set params after
    m.setParam(0, 0.5f);   // input 0 gain 0.5
    m.setParam(1, 0.25f);  // input 1 gain 0.25
    Buf a(1, 64, 1.0f), b(1, 64, 1.0f), out(1, 64);
    AudioBuffer ia = a.view(64), ibuf = b.view(64), ob = out.view(64);
    AudioBuffer ins[2] = {ia, ibuf};
    // process a few blocks so the gain smoothers settle to target
    for (int i = 0; i < 50; ++i) m.process(ins, &ob, 64, TimeInfo{});
    CHECK(close(out.data[0][63], 0.75f, 1e-3f));  // 1·0.5 + 1·0.25
}

AIUDIO_TEST(channel_matrix_default_identity_and_swap) {
    ChannelMatrixNode mat(2, 2);  // identity by default
    mat.prepare(SR, 16);
    Buf in(2, 16), out(2, 16);
    for (std::uint32_t f = 0; f < 16; ++f) { in.data[0][f] = 0.2f; in.data[1][f] = 0.8f; }
    AudioBuffer ib = in.view(16), ob = out.view(16);
    mat.process(&ib, &ob, 16, TimeInfo{});
    CHECK(close(out.data[0][0], 0.2f) && close(out.data[1][0], 0.8f));

    // Swap L<->R: out0 = in1, out1 = in0.
    mat.setParam(0 * 2 + 0, 0.0f); mat.setParam(0 * 2 + 1, 1.0f);
    mat.setParam(1 * 2 + 0, 1.0f); mat.setParam(1 * 2 + 1, 0.0f);
    Buf out2(2, 16);
    AudioBuffer ob2 = out2.view(16);
    mat.process(&ib, &ob2, 16, TimeInfo{});
    CHECK(close(out2.data[0][0], 0.8f) && close(out2.data[1][0], 0.2f));
}

AIUDIO_TEST(delay_pushes_impulse_by_n) {
    DelayNode d(/*maxSec*/ 1.0, /*delayFrames*/ 100, /*feedback*/ 0.0f, /*mix*/ 1.0f, 1);
    d.prepare(SR, 256);
    Buf in(1, 256), out(1, 256);
    in.data[0][0] = 1.0f;  // impulse at 0
    AudioBuffer ib = in.view(256), ob = out.view(256);
    d.process(&ib, &ob, 256, TimeInfo{});
    CHECK(close(out.data[0][0], 0.0f));     // dry suppressed (mix=1)
    CHECK(close(out.data[0][100], 1.0f));   // wet impulse delayed by 100
}

AIUDIO_TEST_MAIN()

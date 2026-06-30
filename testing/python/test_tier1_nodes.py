"""Tier-1 node library at the Python boundary: EQ family, waveshaper, oscillator, noise,
dynamics, delay, pan, mixer, channel matrix, DC blocker, stereo width — plus an end-to-end
chain. Live control uses GraphExecutor.set_param(node, index, value).
"""
from __future__ import annotations

import numpy as np

SR = 48000.0


def aud_compile(g, channels, max_block):
    import aiudio
    ex = aiudio.GraphExecutor()
    assert ex.compile(g, channels=channels, sample_rate=SR, max_block=max_block)
    return ex


def test_biquad_peaking_and_shelf(aud):
    # low shelf +6 dB lifts DC ~2x; peaking far from DC leaves it ~unity
    g = aud.Graph()
    s, sh, k = g.add_source(), g.add_biquad_lowshelf(500.0, 0.707, 6.0, SR), g.add_sink()
    g.connect(s, 0, sh, 0)
    g.connect(sh, 0, k, 0)
    ex = aud_compile(g, 1, 4096)
    out = ex.process(np.ones((1, 4096), np.float32))
    assert abs(float(out[0, -1]) - 1.995) < 0.05

    g2 = aud.Graph()
    s, pk, k = g2.add_source(), g2.add_biquad_peaking(2000.0, 1.0, 9.0, SR), g2.add_sink()
    g2.connect(s, 0, pk, 0)
    g2.connect(pk, 0, k, 0)
    ex2 = aud_compile(g2, 1, 4096)
    out2 = ex2.process(np.ones((1, 4096), np.float32))
    assert abs(float(out2[0, -1]) - 1.0) < 1e-2


def test_waveshaper_saturates(aud):
    g = aud.Graph()
    s, ws, k = g.add_source(), g.add_waveshaper("tanh", 1.0, 1.0), g.add_sink()
    g.connect(s, 0, ws, 0)
    g.connect(ws, 0, k, 0)
    ex = aud_compile(g, 1, 64)
    out = ex.process(np.full((1, 64), 10.0, np.float32))
    assert 0.99 < float(out[0, -1]) <= 1.0  # tanh(10) ≈ 1


def test_oscillator_and_noise_generate(aud):
    for factory in (lambda g: g.add_oscillator("sine", 1000.0, 0.5),
                    lambda g: g.add_noise("white", 0.5)):
        g = aud.Graph()
        n, k = factory(g), g.add_sink()
        g.connect(n, 0, k, 0)
        ex = aud_compile(g, 1, 512)
        out = ex.process(np.zeros((1, 512), np.float32))  # generators ignore input
        assert np.max(np.abs(out)) > 0.1 and np.max(np.abs(out)) <= 0.5001


def test_dc_blocker_removes_dc(aud):
    g = aud.Graph()
    s, dc, k = g.add_source(), g.add_dc_blocker(20.0, 1), g.add_sink()
    g.connect(s, 0, dc, 0)
    g.connect(dc, 0, k, 0)
    ex = aud_compile(g, 1, 4096)
    out = ex.process(np.ones((1, 4096), np.float32))
    assert abs(float(out[0, -1])) < 1e-2


def test_pan_mono_to_stereo(aud):
    g = aud.Graph()
    s, p, k = g.add_source(), g.add_pan(0.0), g.add_sink()
    g.connect(s, 0, p, 0)
    g.connect(p, 0, k, 0)
    ex = aud_compile(g, 2, 64)              # host stereo; pan widens mono→stereo internally
    out = ex.process(np.ones((2, 64), np.float32))
    assert abs(float(out[0, -1]) - 0.7071) < 1e-2
    assert abs(float(out[1, -1]) - 0.7071) < 1e-2


def test_stereo_width_zero_is_mono(aud):
    g = aud.Graph()
    s, w, k = g.add_source(), g.add_stereo_width(0.0), g.add_sink()
    g.connect(s, 0, w, 0)
    g.connect(w, 0, k, 0)
    ex = aud_compile(g, 2, 64)
    x = np.stack([np.ones(64, np.float32), -0.5 * np.ones(64, np.float32)])
    out = ex.process(x)
    assert abs(float(out[0, -1]) - float(out[1, -1])) < 1e-4  # collapsed to mono


def test_delay_impulse(aud):
    g = aud.Graph()
    s, d, k = g.add_source(), g.add_delay(1.0, 100, 0.0, 1.0, 1), g.add_sink()
    g.connect(s, 0, d, 0)
    g.connect(d, 0, k, 0)
    ex = aud_compile(g, 1, 256)
    imp = np.zeros((1, 256), np.float32)
    imp[0, 0] = 1.0
    out = ex.process(imp)
    assert abs(float(out[0, 100]) - 1.0) < 1e-3


def test_channel_matrix_swap(aud):
    g = aud.Graph()
    s, m, k = g.add_source(), g.add_channel_matrix(2, 2), g.add_sink()
    g.connect(s, 0, m, 0)
    g.connect(m, 0, k, 0)
    ex = aud_compile(g, 2, 16)
    x = np.stack([np.full(16, 0.2, np.float32), np.full(16, 0.8, np.float32)])
    assert np.allclose(ex.process(x)[:, 0], [0.2, 0.8])           # identity default
    ex.set_param(m, 0 * 2 + 0, 0.0)
    ex.set_param(m, 0 * 2 + 1, 1.0)
    ex.set_param(m, 1 * 2 + 0, 1.0)
    ex.set_param(m, 1 * 2 + 1, 0.0)
    assert np.allclose(ex.process(x)[:, 0], [0.8, 0.2])           # swapped


def test_compressor_reduces_and_reports_lookahead(aud):
    g = aud.Graph()
    s, c, k = g.add_source(), g.add_compressor(-12.0, 4.0, 1.0, 50.0, 32, 1), g.add_sink()
    g.connect(s, 0, c, 0)
    g.connect(c, 0, k, 0)
    ex = aud_compile(g, 1, 256)
    assert ex.latency_frames == 32                                # lookahead via G9
    loud = np.ones((1, 256), np.float32)
    for _ in range(200):
        out = ex.process(loud)                                    # settle the envelope
    assert 0.2 < float(abs(out[0, -1])) < 0.5                     # compressed ~ -9 dBFS


def test_gate_attenuates_below_threshold(aud):
    g = aud.Graph()
    s, ga, k = g.add_source(), g.add_gate(-30.0, 1.0, 50.0, -80.0), g.add_sink()
    g.connect(s, 0, ga, 0)
    g.connect(ga, 0, k, 0)
    ex = aud_compile(g, 1, 256)
    quiet = np.full((1, 256), 0.005, np.float32)                  # ~-46 dBFS, below threshold
    for _ in range(200):
        out = ex.process(quiet)
    assert float(abs(out[0, -1])) < 0.005 * 0.1                   # gated down


def test_end_to_end_chain(aud):
    g = aud.Graph()
    osc = g.add_oscillator("saw", 220.0, 0.6)
    ws = g.add_waveshaper("tanh", 2.0, 1.0)
    eq = g.add_biquad_peaking(2000.0, 1.0, 6.0, SR)
    comp = g.add_compressor(-18.0, 4.0, 5.0, 80.0, 16, 2)
    pan = g.add_pan(0.2)
    width = g.add_stereo_width(1.2)
    k = g.add_sink()
    for a, b in [(osc, ws), (ws, eq), (eq, comp), (comp, pan), (pan, width), (width, k)]:
        assert g.connect(a, 0, b, 0)
    ok, err = g.validate()
    assert ok, err
    ex = aud_compile(g, 2, 128)
    out = ex.process_multi([np.zeros((2, 128), np.float32)])[0]
    assert out.shape == (2, 128)
    assert np.all(np.isfinite(out)) and np.max(np.abs(out)) > 0.0

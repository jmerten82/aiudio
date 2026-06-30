"""Multi-band parametric EQ (ParametricEqNode) at the Python boundary: a whole EQ as one node,
built from a list of (type, freq, q, gain_db) bands, with per-band live control."""
from __future__ import annotations

import numpy as np

SR = 48000.0


def _dc(ex, n=4096):
    return float(ex.process(np.ones((1, n), np.float32))[0, -1])


def test_three_band_eq_low_shelf_boosts_dc(aud):
    g = aud.Graph()
    s = g.add_source()
    eq = g.add_parametric_eq(
        [("lowshelf", 200.0, 0.707, 6.0),   # +6 dB at DC
         ("peaking", 2000.0, 1.0, 0.0),     # flat
         ("highshelf", 8000.0, 0.707, 0.0)],  # flat
        SR,
    )
    k = g.add_sink()
    g.connect(s, 0, eq, 0)
    g.connect(eq, 0, k, 0)
    assert g.node_type(eq) == "ParametricEqNode"
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=4096)
    assert abs(_dc(ex) - 1.995) < 0.05            # 10^(6/20) ≈ 1.995


def test_per_band_live_control(aud):
    g = aud.Graph()
    s = g.add_source()
    eq = g.add_parametric_eq([("lowshelf", 200.0, 0.707, 0.0)], SR)  # starts flat
    k = g.add_sink()
    g.connect(s, 0, eq, 0)
    g.connect(eq, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=4096)
    assert abs(_dc(ex) - 1.0) < 1e-2                              # flat
    ex.set_param(eq, 0 * 3 + 2, 6.0)                             # band 0, gain_db → +6
    for _ in range(4):
        dc = _dc(ex)
    assert abs(dc - 1.995) < 0.05                                # boosted live


def test_single_band_matches_standalone_biquad(aud):
    # A 1-band peaking EQ produces the same DC level as the standalone peaking biquad factory.
    def dc_of(graph_builder):
        g = aud.Graph()
        s = g.add_source()
        node = graph_builder(g)
        k = g.add_sink()
        g.connect(s, 0, node, 0)
        g.connect(node, 0, k, 0)
        ex = aud.GraphExecutor()
        assert ex.compile(g, channels=1, sample_rate=SR, max_block=4096)
        return _dc(ex)

    a = dc_of(lambda g: g.add_parametric_eq([("peaking", 1500.0, 1.0, 6.0)], SR))
    b = dc_of(lambda g: g.add_biquad_peaking(1500.0, 1.0, 6.0, SR))
    assert abs(a - b) < 1e-5

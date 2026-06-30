"""Per-port channel counts (G8): nodes that change the channel width.

A graph compiled at a host channel count can now contain interior ports of *different*
widths — proven with a down-mix (2->1) and an up-mix (1->2). The numpy I/O boundary stays
at the host width; the width change happens inside the graph.
"""
from __future__ import annotations

import numpy as np

SR = 48000.0


def _stereo(left: float, right: float, frames: int = 16) -> np.ndarray:
    x = np.zeros((2, frames), np.float32)
    x[0] = left
    x[1] = right
    return x


def test_downmix_collapses_to_mono(aud):
    g = aud.Graph()
    s, d, k = g.add_source(), g.add_downmix(), g.add_sink()
    g.connect(s, 0, d, 0)
    g.connect(d, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=2, sample_rate=SR, max_block=64)  # host = stereo

    y = ex.process(_stereo(1.0, 0.5))
    assert np.allclose(y[0], 0.75)  # (L + R) / 2
    assert np.allclose(y[1], 0.0)   # ch1 silent → mono at the downmix


def test_upmix_after_downmix_duplicates(aud):
    g = aud.Graph()
    s, d, u, k = g.add_source(), g.add_downmix(), g.add_upmix(2), g.add_sink()
    g.connect(s, 0, d, 0)
    g.connect(d, 0, u, 0)
    g.connect(u, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=2, sample_rate=SR, max_block=64)

    y = ex.process(_stereo(1.0, 0.5))
    assert np.allclose(y[0], 0.75)  # mono duplicated to both channels
    assert np.allclose(y[1], 0.75)


def test_uniform_width_unchanged(aud):
    g = aud.Graph()
    s, gn, k = g.add_source(), g.add_gain(0.5), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=2, sample_rate=SR, max_block=64)

    y = ex.process(_stereo(1.0, 0.5))
    assert np.allclose(y[0], 0.5)   # both channels processed independently (back-compat)
    assert np.allclose(y[1], 0.25)

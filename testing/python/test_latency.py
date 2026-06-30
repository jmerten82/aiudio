"""Latency reporting + delay compensation (G9, PDC) from Python.

A node declares its latency (`add_latency`), the executor reports the graph total
(`latency_frames`), and parallel branches of differing latency are auto-aligned so they
recombine in phase.
"""
from __future__ import annotations

import numpy as np

SR = 48000.0


def test_latency_reported(aud):
    g = aud.Graph()
    s, lat, k = g.add_source(), g.add_latency(64, 1), g.add_sink()
    g.connect(s, 0, lat, 0)
    g.connect(lat, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=32)
    assert ex.latency_frames == 64


def test_parallel_paths_realign_in_phase(aud):
    # source -> [latency(8) -> sum0] + [-> sum1] -> sink
    g = aud.Graph()
    s = g.add_source()
    lat = g.add_latency(8, 1)
    sm = g.add_sum(2)
    k = g.add_sink()
    g.connect(s, 0, lat, 0)
    g.connect(lat, 0, sm, 0)
    g.connect(s, 0, sm, 1)   # direct branch → executor compensates by 8
    g.connect(sm, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=32)
    assert ex.latency_frames == 8

    x = np.zeros((1, 32), np.float32)
    x[0, 0] = 1.0  # impulse
    y = ex.process(x)[0]
    assert np.isclose(y[0], 0.0)   # no early uncompensated impulse
    assert np.isclose(y[8], 2.0)   # both branches aligned at frame 8 → 2x
    assert np.allclose(np.delete(y, 8), 0.0)  # single impulse, everywhere else 0


def test_zero_latency_unchanged(aud):
    g = aud.Graph()
    s, gn, k = g.add_source(), g.add_gain(0.5), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=32)
    assert ex.latency_frames == 0
    assert np.allclose(ex.process(np.ones((1, 16), np.float32)), 0.5)  # gain, no delay

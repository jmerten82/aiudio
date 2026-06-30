"""xrun/underrun policy + telemetry (M9.1) from Python.

The executor counts blocks it can't fully render and degrades them to silence (never
garbage); control-command overflow is counted as dropped commands. All RT-safe.
"""
from __future__ import annotations

import numpy as np

SR = 48000.0


def _gain_graph(aud, gain=1.0):
    g = aud.Graph()
    s, gn, k = g.add_source(), g.add_gain(gain), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    return g, gn


def test_clean_run_has_zero_xruns(aud):
    g, _ = _gain_graph(aud, 0.5)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=128)
    for _ in range(50):
        ex.process(np.ones((1, 64), np.float32))
    assert ex.xrun_count == 0


def test_oversized_block_degrades_to_silence_and_counts(aud):
    g, _ = _gain_graph(aud, 1.0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)  # max 64
    y = ex.process(np.ones((1, 128), np.float32))                   # ask for 128
    assert np.allclose(y[0, :64], 1.0)    # rendered region
    assert np.allclose(y[0, 64:], 0.0)    # tail silenced, not garbage
    assert ex.xrun_count == 1


def test_dropped_commands_counted(aud):
    g, gn = _gain_graph(aud, 1.0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)
    assert ex.dropped_commands == 0
    for _ in range(8192):          # flood the lock-free queue without draining (no process())
        ex.set_gain(gn, 0.5)
    assert ex.dropped_commands > 0

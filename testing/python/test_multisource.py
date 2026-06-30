"""MultiSourceManager (M10): N input sources + M output sinks composed on one clock.

Producers push numpy into input streams, the pump runs the multi-stream graph, consumers
pop numpy from output streams — each (stream, channel) crossing via its own lock-free ring.
"""
from __future__ import annotations

import numpy as np

SR = 48000.0


def _mix_graph(aud):
    g = aud.Graph()
    s0, s1, sm, k = g.add_source(0), g.add_source(1), g.add_sum(2), g.add_sink(0)
    g.connect(s0, 0, sm, 0)
    g.connect(s1, 0, sm, 1)
    g.connect(sm, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)
    return ex


def test_two_inputs_mix_to_one_output(aud):
    ex = _mix_graph(aud)
    mgr = aud.MultiSourceManager(num_inputs=2, num_outputs=1, channels=1, max_block=64, ring_frames=256)
    assert (mgr.num_inputs, mgr.num_outputs, mgr.channels) == (2, 1, 1)
    mgr.push_input(0, np.full((1, 32), 0.5, np.float32))
    mgr.push_input(1, np.full((1, 32), 0.3, np.float32))
    mgr.process(ex, 32)
    out = mgr.pop_output(0, 32)
    assert out.shape == (1, 32)
    assert np.allclose(out, 0.8)


def test_one_input_two_outputs(aud):
    g = aud.Graph()
    s = g.add_source(0)
    g0, g1 = g.add_gain(0.5), g.add_gain(0.25)
    k0, k1 = g.add_sink(0), g.add_sink(1)
    g.connect(s, 0, g0, 0)
    g.connect(s, 0, g1, 0)
    g.connect(g0, 0, k0, 0)
    g.connect(g1, 0, k1, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)

    mgr = aud.MultiSourceManager(1, 2, 1, 64, 256)
    mgr.push_input(0, np.ones((1, 32), np.float32))
    mgr.process(ex, 32)
    assert np.allclose(mgr.pop_output(0, 32), 0.5)
    assert np.allclose(mgr.pop_output(1, 32), 0.25)


def test_input_underrun_silence_and_telemetry(aud):
    ex = _mix_graph(aud)
    mgr = aud.MultiSourceManager(2, 1, 1, 64, 256)
    mgr.process(ex, 32)  # nothing pushed
    assert np.allclose(mgr.pop_output(0, 32), 0.0)   # silence, not garbage
    assert mgr.input_underruns(0) > 0
    assert mgr.input_underruns(1) > 0


def test_output_overrun_telemetry(aud):
    ex = _mix_graph(aud)
    mgr = aud.MultiSourceManager(2, 1, 1, 64, ring_frames=128)  # small output ring
    for _ in range(8):  # feed + pump, never pop
        mgr.push_input(0, np.ones((1, 64), np.float32))
        mgr.push_input(1, np.ones((1, 64), np.float32))
        mgr.process(ex, 64)
    assert mgr.output_overruns(0) > 0

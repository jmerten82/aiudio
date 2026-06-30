"""Multi-stream executor (G10): N input streams + M output streams from Python.

Each SourceNode/SinkNode binds to a stream index; `process_multi` takes a list of input
arrays (one per input stream) and returns a list of output arrays (one per output stream).
The single-stream `process` is the back-compatible 1-stream special case.
"""
from __future__ import annotations

import numpy as np

SR = 48000.0


def test_two_input_streams_route_and_mix(aud):
    g = aud.Graph()
    s0, s1 = g.add_source(stream=0), g.add_source(stream=1)
    g0, g1 = g.add_gain(0.5), g.add_gain(0.25)  # asymmetric → proves input k → source k
    sm, k = g.add_sum(2), g.add_sink(stream=0)
    g.connect(s0, 0, g0, 0)
    g.connect(s1, 0, g1, 0)
    g.connect(g0, 0, sm, 0)
    g.connect(g1, 0, sm, 1)
    g.connect(sm, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=128)
    assert ex.input_streams == 2
    assert ex.output_streams == 1
    assert ex.channels == 1

    a = np.ones((1, 64), np.float32)
    b = 2.0 * np.ones((1, 64), np.float32)
    out = ex.process_multi([a, b])
    assert len(out) == 1
    assert np.allclose(out[0], 1.0 * 0.5 + 2.0 * 0.25)  # = 1.0


def test_two_output_streams_route_distinctly(aud):
    g = aud.Graph()
    s = g.add_source(stream=0)
    ga, gb = g.add_gain(0.5), g.add_gain(0.25)
    ka, kb = g.add_sink(stream=0), g.add_sink(stream=1)
    g.connect(s, 0, ga, 0)
    g.connect(s, 0, gb, 0)
    g.connect(ga, 0, ka, 0)
    g.connect(gb, 0, kb, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=128)
    assert ex.output_streams == 2

    out = ex.process_multi([np.ones((1, 32), np.float32)], num_outputs=2)
    assert len(out) == 2
    assert np.allclose(out[0], 0.5)
    assert np.allclose(out[1], 0.25)


def test_num_outputs_defaults_to_output_streams(aud):
    g = aud.Graph()
    s = g.add_source(stream=0)
    ka, kb = g.add_sink(stream=0), g.add_sink(stream=1)
    g.connect(s, 0, ka, 0)
    g.connect(s, 0, kb, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)
    out = ex.process_multi([np.ones((1, 16), np.float32)])  # no num_outputs → infer 2
    assert len(out) == 2


def test_single_process_back_compatible(aud):
    g = aud.Graph()
    s, gn, k = g.add_source(), g.add_gain(0.5), g.add_sink()  # default stream 0
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=128)
    assert np.allclose(ex.process(np.ones((1, 64), np.float32)), 0.5)  # unchanged

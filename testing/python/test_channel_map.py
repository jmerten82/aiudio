"""Boundary channel mapping (M9.2) at the Python boundary: map_channels.

Device↔graph mono↔stereo / N↔M mapping at the I/O edge (the complement to the graph's own
DownmixNode/UpmixNode, which change width *inside* the graph).
"""
from __future__ import annotations

import numpy as np


def test_mono_to_stereo_duplicates(aud):
    mono = np.full((1, 8), 0.5, np.float32)
    out = aud.map_channels(mono, 2)
    assert out.shape == (2, 8)
    assert np.allclose(out, 0.5)


def test_stereo_to_mono_averages(aud):
    stereo = np.stack([np.full(4, 1.0, np.float32), np.full(4, 0.4, np.float32)])
    out = aud.map_channels(stereo, 1)
    assert out.shape == (1, 4)
    assert np.allclose(out[0], 0.7)  # (1.0 + 0.4) / 2


def test_wider_dst_zero_pads(aud):
    stereo = np.full((2, 4), 0.3, np.float32)
    out = aud.map_channels(stereo, 3)
    assert np.allclose(out[0], 0.3) and np.allclose(out[1], 0.3)
    assert np.allclose(out[2], 0.0)  # extra dst channel silenced


def test_explicit_modes(aud):
    stereo = np.stack([np.full(4, 0.2, np.float32), np.full(4, 0.8, np.float32)])
    dup = aud.map_channels(stereo, 2, aud.ChannelMapMode.DuplicateMono)
    assert np.allclose(dup[0], 0.2) and np.allclose(dup[1], 0.2)  # ch0 → both
    down = aud.map_channels(stereo, 1, aud.ChannelMapMode.DownmixToMono)
    assert np.allclose(down[0], 0.5)  # (0.2 + 0.8) / 2


def test_boundary_use_mono_mic_into_stereo_graph(aud):
    """A mono 'device' block mapped up to a stereo graph, processed, mapped back down."""
    SR = 48000.0
    g = aud.Graph()
    s, gn, k = g.add_source(), g.add_gain(0.5), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=2, sample_rate=SR, max_block=64)

    mic = np.full((1, 64), 0.8, np.float32)          # mono device capture
    stereo_in = aud.map_channels(mic, 2)             # boundary: mono → stereo graph
    stereo_out = ex.process(stereo_in)               # graph runs at 2ch, gain 0.5
    mono_out = aud.map_channels(stereo_out, 1)       # boundary: stereo graph → mono device
    assert np.allclose(mono_out[0], 0.4)             # 0.8 → gain 0.5 → 0.4, round-tripped

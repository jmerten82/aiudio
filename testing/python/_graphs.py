"""Shared graph + signal builders for the Phase-0 Python test suites.

Kept tiny and dependency-free (numpy only) so every test reads the same way. `aiudio`
is passed in rather than imported here, so a single `pytest.importorskip("aiudio")`
in the fixtures controls skipping when the extension isn't built.
"""
from __future__ import annotations

import numpy as np

SR = 48000.0
BLOCK = 512


def gain_chain(aiudio, gain: float = 0.5):
    """Source -> Gain -> Sink. Returns (graph, gain_id) — the simplest exact-math rig."""
    g = aiudio.Graph()
    s = g.add_source()
    gn = g.add_gain(gain)
    k = g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    return g, gn


def dsp_chain(aiudio, sr: float = SR):
    """Source -> Biquad LP(1k) -> Gain(0.5) -> Meter -> Sink. Returns (graph, ids)."""
    g = aiudio.Graph()
    s = g.add_source()
    lp = g.add_biquad_lowpass(1000.0, 0.707, sr)
    gn = g.add_gain(0.5)
    m = g.add_meter()
    k = g.add_sink()
    g.connect(s, 0, lp, 0)
    g.connect(lp, 0, gn, 0)
    g.connect(gn, 0, m, 0)
    g.connect(m, 0, k, 0)
    return g, {"lp": lp, "gain": gn, "meter": m}


def two_tone(n: int, sr: float = SR, f1: float = 300.0, f2: float = 5000.0, amp: float = 0.4):
    """A 1-D float32 mix of two sinusoids (a low tone that passes a 1k LP + a high one)."""
    t = np.arange(n) / sr
    return (amp * np.sin(2 * np.pi * f1 * t) + amp * np.sin(2 * np.pi * f2 * t)).astype(np.float32)


def process_stream(ex, sig, block: int = BLOCK):
    """Run a 1-D signal through an executor block by block; return the 1-D output."""
    out = np.zeros(len(sig), np.float32)
    for i in range(0, len(sig), block):
        b = sig[i:i + block].reshape(1, -1)
        out[i:i + b.shape[1]] = ex.process(b)[0]
    return out


def rms(x) -> float:
    return float(np.sqrt(np.mean(np.square(x))))

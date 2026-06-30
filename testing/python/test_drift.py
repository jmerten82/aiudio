"""Drift compensation (M9.5) at the Python boundary: ResamplingSource.

A producer push()es source-rate audio; the engine pull()s a fixed engine-rate block. The
ring-fill servo nudges the resample ratio so a drifting source stays bounded and aligned
(ADR-0015). These mirror the C++ soak tests at the numpy boundary.
"""
from __future__ import annotations

import numpy as np


def _soak(aud, true_ratio, steps=40000, block=64, ring=4096):
    """Drive a drifting producer into a ResamplingSource; return (src, max_fill, clean)."""
    src = aud.ResamplingSource(channels=1, nominal_ratio=1.0, ring_frames=ring, max_block=block)
    acc, ppos, max_fill, clean = 0.0, 0, 0, True
    for step in range(steps):
        acc += block * true_ratio
        n = int(acc)
        acc -= n
        idx = np.arange(ppos, ppos + n)
        src.push(np.sin(0.02 * idx).astype(np.float32)[None, :])
        ppos += n
        out = src.pull(block)
        if step > 3000:
            max_fill = max(max_fill, src.fill_frames)
            if not np.all(np.isfinite(out)) or np.max(np.abs(out)) > 1.5:
                clean = False
    return src, max_fill, clean


def test_faster_producer_no_overrun(aud):
    src, max_fill, clean = _soak(aud, true_ratio=1.003)  # producer 0.3% fast
    assert src.overruns == 0      # ring never overflowed
    assert max_fill < 4096        # stayed inside the ring
    assert src.ratio > 1.0        # servo sped consumption up
    assert clean


def test_slower_producer_keeps_ratio_below_one(aud):
    src, _, clean = _soak(aud, true_ratio=0.997)  # producer 0.3% slow
    assert src.ratio < 1.0        # servo slowed consumption to match
    assert clean


def test_no_drift_preserves_energy(aud):
    src = aud.ResamplingSource(channels=1, nominal_ratio=1.0, ring_frames=2048, max_block=64)
    in_sq = out_sq = n = 0
    ppos = 0
    for step in range(3000):
        idx = np.arange(ppos, ppos + 64)
        x = (0.5 * np.sin(0.05 * idx)).astype(np.float32)[None, :]
        src.push(x)
        ppos += 64
        y = src.pull(64)
        if step > 500:
            in_sq += float((x[0] ** 2).sum())
            out_sq += float((y[0] ** 2).sum())
            n += 64
    in_rms, out_rms = (in_sq / n) ** 0.5, (out_sq / n) ** 0.5
    assert abs(in_rms - out_rms) < 0.05  # ratio ≈ 1 → energy preserved


def test_telemetry_surface(aud):
    src = aud.ResamplingSource(channels=2, nominal_ratio=1.001, ring_frames=1024, max_block=32)
    assert src.channels == 2
    assert abs(src.nominal_ratio - 1.001) < 1e-9
    assert src.underruns >= 0 and src.overruns >= 0

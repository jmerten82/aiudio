"""Boundary resampler (M9.3) at the Python (numpy) boundary.

The same C++ streaming SRC, driven from numpy: ratio = input_rate / output_rate. The
instance is stateful, so feeding consecutive blocks is seamless. These mirror the C++
tests (DC/length/frequency preserved, live ratio, reported latency) at the binding.
"""
from __future__ import annotations

import numpy as np


def _resample_all(aud, x, ratio):
    """Push a whole signal (channels, frames) through in one call; return (channels, produced)."""
    rs = aud.Resampler(channels=x.shape[0], ratio=ratio)
    out_cap = int(x.shape[1] / ratio) + 8
    out, consumed = rs.process(np.ascontiguousarray(x, dtype=np.float32), out_cap)
    return out, consumed


def test_dc_is_preserved(aud):
    x = np.full((1, 400), 0.7, dtype=np.float32)
    out, _ = _resample_all(aud, x, 44100.0 / 48000.0)
    assert out.shape[0] == 1
    assert np.allclose(out[0, 20:], 0.7, atol=1e-4)


def test_length_scales_with_ratio(aud):
    x = np.zeros((1, 4800), dtype=np.float32)
    up, _ = _resample_all(aud, x, 44100.0 / 48000.0)   # upsample → more frames
    down, _ = _resample_all(aud, x, 48000.0 / 44100.0)  # downsample → fewer
    assert up.shape[1] > x.shape[1]
    assert down.shape[1] < x.shape[1]
    assert abs(up.shape[1] - 4800 * 48000 // 44100) < 4


def test_sine_frequency_preserved(aud):
    n = 4410
    t = np.arange(n)
    x = np.sin(2 * np.pi * 0.05 * t).astype(np.float32)[None, :]  # ~2205 Hz @ 44.1k
    out, _ = _resample_all(aud, x, 44100.0 / 48000.0)

    def crossings(v, frm):
        s = v[frm:]
        return int(np.sum((s[:-1] <= 0) != (s[1:] <= 0)))

    assert abs(crossings(x[0], 0) - crossings(out[0], 10)) <= 3


def test_set_ratio_and_props(aud):
    rs = aud.Resampler(channels=2, ratio=1.0)
    assert rs.channels == 2
    assert rs.latency_frames >= 1
    rs.set_ratio(1.05)
    assert abs(rs.ratio - 1.05) < 1e-9
    rs.set_ratio(1000.0)  # clamped to kMaxRatio (16)
    assert rs.ratio <= 16.0 + 1e-9


def test_streaming_is_seamless(aud):
    """Chunked feeding == one-shot, proving block boundaries are seamless."""
    ratio = 44100.0 / 48000.0
    x = np.sin(0.03 * np.arange(1000)).astype(np.float32)[None, :]
    whole, _ = _resample_all(aud, x, ratio)

    rs = aud.Resampler(channels=1, ratio=ratio)
    pieces = []
    off, chunk = 0, 37
    while off < x.shape[1]:
        n = min(chunk, x.shape[1] - off)
        out, _ = rs.process(np.ascontiguousarray(x[:, off:off + n]), 128)
        pieces.append(out)
        off += n
    chunked = np.concatenate(pieces, axis=1)
    m = whole.shape[1]
    assert np.allclose(whole[0], chunked[0, :m], atol=1e-6)


def test_channels_are_independent(aud):
    rs = aud.Resampler(channels=2, ratio=0.5)
    x = np.stack([np.full(64, 0.4, np.float32), np.full(64, -0.9, np.float32)])
    out, _ = rs.process(np.ascontiguousarray(x), 256)
    assert np.allclose(out[0, 20:], 0.4, atol=1e-4)
    assert np.allclose(out[1, 20:], -0.9, atol=1e-4)

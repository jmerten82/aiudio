"""'One IR, many backends' + the RT-safe control plane (headless, deterministic).

The headline check is architecture invariant #3 (ADR-0009): the SAME compiled graph
must produce the SAME output on the numpy executor and on the offline WAV backend.
The rest exercise the G7 control queue + telemetry without needing a device.
"""
from __future__ import annotations

import wave

import numpy as np

from _graphs import BLOCK, SR, dsp_chain, gain_chain, process_stream, rms, two_tone


def _write_wav_i16(path, sig, sr=int(SR)):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes((np.clip(sig, -1, 1) * 32767).astype("<i2").tobytes())


def _read_wav_i16(path):
    with wave.open(str(path), "rb") as r:
        return np.frombuffer(r.readframes(r.getnframes()), "<i2").astype(np.float32) / 32768.0


def test_numpy_matches_offline_backend(aud, tmp_path):
    """Invariant #3: numpy executor == offline WAV backend for the same graph."""
    sig = two_tone(int(SR))  # 1 s

    g_np, _ = dsp_chain(aud)
    ex_np = aud.GraphExecutor()
    assert ex_np.compile(g_np, channels=1, sample_rate=SR, max_block=BLOCK)
    out_np = process_stream(ex_np, sig)

    in_wav, out_wav = tmp_path / "in.wav", tmp_path / "out.wav"
    _write_wav_i16(in_wav, sig)
    g_off, _ = dsp_chain(aud)
    ex_off = aud.GraphExecutor()
    assert ex_off.compile(g_off, channels=1, sample_rate=SR, max_block=BLOCK)
    ob = aud.OfflineBackend(str(in_wav), str(out_wav), aud.WavFormat.Int16)
    assert ob.open(ex_off, block_size=BLOCK)
    ob.start()
    out_off = _read_wav_i16(out_wav)

    n = min(len(out_np), len(out_off))
    assert n > 0
    max_diff = float(np.max(np.abs(out_np[:n] - out_off[:n])))
    assert max_diff < 2e-3, f"numpy vs offline diverged by {max_diff:.2e} (> int16 quantum)"


def test_set_gain_applies_on_next_block(aud):
    g, gn = gain_chain(aud, 0.5)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)
    dc = np.ones((1, 64), np.float32)
    assert np.allclose(ex.process(dc), 0.5)
    assert ex.set_gain(gn, 0.25)            # enqueue (False only if the queue is full)
    assert np.allclose(ex.process(dc), 0.25)  # applied at the next block
    assert ex.render_count >= 2             # telemetry


def test_set_cutoff_opens_the_filter(aud):
    g = aud.Graph()
    s = g.add_source()
    lp = g.add_biquad_lowpass(500.0, 0.707, SR)
    k = g.add_sink()
    g.connect(s, 0, lp, 0)
    g.connect(lp, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=256)

    t = np.arange(256) / SR
    tone = np.sin(2 * np.pi * 6000 * t).astype(np.float32).reshape(1, 256)
    for _ in range(8):
        lo = ex.process(tone)               # settle at cutoff 500 Hz
    assert ex.set_cutoff(lp, 18000.0)
    for _ in range(8):
        hi = ex.process(tone)               # settle at cutoff 18 kHz
    assert rms(hi) > 3 * rms(lo)            # the 6 kHz tone now passes


def test_validate_rejects_a_cycle(aud):
    g = aud.Graph()
    a, b = g.add_gain(1.0), g.add_gain(1.0)
    g.connect(a, 0, b, 0)
    g.connect(b, 0, a, 0)
    ok, err = g.validate()
    assert not ok and "cycle" in err.lower()


def test_meter_publishes_level(aud):
    g, ids = dsp_chain(aud)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=1024)
    ex.process(two_tone(1024).reshape(1, 1024))
    assert g.meter_mean_square(ids["meter"]) > 0.0

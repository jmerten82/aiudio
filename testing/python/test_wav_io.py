"""WAV file read/write at the Python (numpy) boundary (M6).

The same C++ WavReader/WavWriter the offline backend uses, exposed as the numpy
boundary for offline/tooling I/O. These are file I/O — control/offline thread only,
never the audio thread (ADR-0004). This is the non-RT foundation for a live recorder
(drain a lock-free ring into a WavWriter on a writer thread).

Covered: exact float32 round-trip, Int16 quantization, planar (channels, frames)
convention, streaming partial reads with end-of-data signaling, the RAII destructor
finalize (a dropped writer still yields a readable file), the `with` context manager,
channel mismatch handling, and the not-ok guards.
"""
from __future__ import annotations

import gc

import numpy as np

SR = 48000.0


def _signal(channels: int, frames: int) -> np.ndarray:
    """A distinct, in-range planar test signal per channel."""
    n = np.arange(frames)
    rows = [0.5 * np.sin(2 * np.pi * (220.0 * (c + 1)) * n / SR) for c in range(channels)]
    return np.stack(rows).astype(np.float32)


def test_float32_round_trip_is_exact(aud, tmp_path):
    p = str(tmp_path / "f32.wav")
    x = _signal(2, 1000)
    w = aud.WavWriter(p, channels=2, sample_rate=SR, format=aud.WavFormat.Float32)
    w.write(x)
    w.finalize()

    r = aud.WavReader(p)
    assert r.ok and r.channels == 2 and r.sample_rate == SR and r.total_frames == 1000
    got = r.read(10_000)
    assert got.shape == (2, 1000)
    assert np.array_equal(got, x)  # 32-bit float is lossless


def test_int16_round_trip_is_close(aud, tmp_path):
    p = str(tmp_path / "i16.wav")
    x = _signal(1, 2000)
    with aud.WavWriter(p, channels=1, sample_rate=SR, format=aud.WavFormat.Int16) as w:
        w.write(x)
    got = aud.WavReader(p).read(4000)
    assert got.shape == (1, 2000)
    assert np.max(np.abs(got - x)) < 4e-5  # within one 16-bit quantum


def test_multiple_writes_append(aud, tmp_path):
    p = str(tmp_path / "chunks.wav")
    x = _signal(2, 900)
    with aud.WavWriter(p, channels=2, sample_rate=SR, format=aud.WavFormat.Float32) as w:
        for start in range(0, 900, 128):  # ragged block sizes
            w.write(np.ascontiguousarray(x[:, start:start + 128]))
    got = aud.WavReader(p).read(2000)
    assert np.array_equal(got, x)


def test_streaming_partial_reads_signal_end_of_data(aud, tmp_path):
    p = str(tmp_path / "stream.wav")
    x = _signal(2, 1000)
    with aud.WavWriter(p, channels=2, sample_rate=SR) as w:
        w.write(x)

    r = aud.WavReader(p)
    a = r.read(600)
    b = r.read(600)   # only 400 remain
    c = r.read(600)   # end of data
    assert a.shape == (2, 600) and b.shape == (2, 400) and c.shape == (2, 0)
    assert np.array_equal(np.concatenate([a, b], axis=1), x)


def test_context_manager_finalizes(aud, tmp_path):
    p = str(tmp_path / "ctx.wav")
    with aud.WavWriter(p, channels=1, sample_rate=SR) as w:
        assert w.ok
        w.write(np.full((1, 64), 0.3, np.float32))
    r = aud.WavReader(p)   # readable immediately after the block exits
    assert r.ok and r.total_frames == 64
    assert np.allclose(r.read(64), 0.3, atol=1e-6)


def test_destructor_finalizes_without_explicit_call(aud, tmp_path):
    """A dropped writer (no finalize(), no `with`) must still leave a readable WAV — RAII."""
    p = str(tmp_path / "dropped.wav")
    w = aud.WavWriter(p, channels=1, sample_rate=SR)
    w.write(np.full((1, 50), 0.25, np.float32))
    del w
    gc.collect()
    r = aud.WavReader(p)
    assert r.ok and r.total_frames == 50


def test_channel_mismatch_is_zero_filled(aud, tmp_path):
    """Writing fewer channels than declared zero-fills the rest (per WavWriter contract)."""
    p = str(tmp_path / "mono_into_stereo.wav")
    with aud.WavWriter(p, channels=2, sample_rate=SR) as w:
        w.write(np.full((1, 100), 0.4, np.float32))  # 1 ch into a 2-ch file
    got = aud.WavReader(p).read(100)
    assert got.shape == (2, 100)
    assert np.allclose(got[0], 0.4) and np.allclose(got[1], 0.0)


def test_missing_file_reader_is_not_ok(aud, tmp_path):
    r = aud.WavReader(str(tmp_path / "does_not_exist.wav"))
    assert not r.ok
    assert r.read(100).shape == (0, 0)  # graceful, no crash


def test_records_graph_output_to_wav(aud, tmp_path):
    """The intended use: capture a graph's processed output (off the audio thread) to a WAV.
    Precursor to the live recorder — here the 'clock' is a Python loop over ex.process()."""
    g = aud.Graph()
    src = g.add_source()
    gn = g.add_gain(0.5)
    k = g.add_sink()
    g.connect(src, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    assert g.validate()[0]
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=256)

    p = str(tmp_path / "rendered.wav")
    frames = 256
    with aud.WavWriter(p, channels=1, sample_rate=SR, format=aud.WavFormat.Float32) as w:
        for _ in range(8):
            out = ex.process(np.ones((1, frames), np.float32))  # C++ runs the graph
            w.write(np.ascontiguousarray(out))                  # Python writes off-thread
    r = aud.WavReader(p)
    assert r.total_frames == 8 * frames
    assert np.allclose(r.read(8 * frames), 0.5, atol=1e-6)  # gain 0.5 applied

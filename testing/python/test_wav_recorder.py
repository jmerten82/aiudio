"""Live WAV recorder on LiveMultiSource — record the mixed master output to a file.

The audio thread (the master pump) taps each output block into a lock-free ring; a writer
thread drains it to disk (ADR-0004 — Python/file I/O never touch the audio thread). Headless
via MockBackends so it runs anywhere; a live-gated test records a real mic to a WAV.
"""
from __future__ import annotations

import time

import numpy as np
import pytest

SR = 48000.0
BLOCK = 64


def _mix_lms(aud, channels=1):
    """Two sources → sum → sink(0), compiled + wrapped in a LiveMultiSource."""
    g = aud.Graph()
    s0, s1 = g.add_source(0), g.add_source(1)
    mx, k = g.add_sum(2), g.add_sink(0)
    g.connect(s0, 0, mx, 0)
    g.connect(s1, 0, mx, 1)
    g.connect(mx, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=channels, sample_rate=SR, max_block=BLOCK)
    return aud.LiveMultiSource(ex, engine_rate=SR, channels=channels, max_block=BLOCK)


def test_records_master_mix_to_wav(aud, tmp_path):
    lms = _mix_lms(aud)
    a_be, b_be, master = aud.MockBackend(), aud.MockBackend(), aud.MockBackend()
    assert lms.attach_source(a_be, stream=0, source_rate=SR, channels=1, block_size=BLOCK)
    assert lms.attach_source(b_be, stream=1, source_rate=SR, channels=1, block_size=BLOCK)
    assert lms.attach_master_output(master, out_stream=0, channels=1, sample_rate=SR, block_size=BLOCK)
    a_be.set_input_value(0.5)
    b_be.set_input_value(0.2)

    path = str(tmp_path / "mix.wav")
    assert lms.attach_wav_recorder(path, format=aud.WavFormat.Float32)
    assert lms.recording

    for be in (a_be, b_be, master):
        be.start()
    for _ in range(500):
        for be in (a_be, b_be, master):
            be.tick(BLOCK)
    lms.stop_recording()

    assert lms.recorded_frames == 500 * BLOCK
    assert lms.record_dropped_frames == 0
    r = aud.WavReader(path)
    assert r.ok and r.channels == 1 and r.total_frames == 500 * BLOCK
    data = r.read(r.total_frames)
    assert np.isclose(data[0, -1], 0.7, atol=1e-2)  # 0.5 + 0.2 mix landed on disk


def test_double_attach_is_rejected(aud, tmp_path):
    lms = _mix_lms(aud)
    m = aud.MockBackend()
    assert lms.attach_master_output(m, out_stream=0, channels=1, sample_rate=SR, block_size=BLOCK)
    assert lms.attach_wav_recorder(str(tmp_path / "a.wav"))
    assert lms.attach_wav_recorder(str(tmp_path / "b.wav")) is False  # already recording
    lms.stop_recording()
    lms.stop_recording()  # idempotent, no crash


def test_recorder_off_clock_source_still_records(aud, tmp_path):
    """A 44.1 kHz source drift-compensated onto the 48 kHz engine, then recorded — proves the
    tap sees the post-resample mix, not the raw source rate."""
    lms = _mix_lms(aud)
    a_be, b_be, master = aud.MockBackend(), aud.MockBackend(), aud.MockBackend()
    assert lms.attach_source(a_be, stream=0, source_rate=SR, channels=1, block_size=BLOCK)
    assert lms.attach_source(b_be, stream=1, source_rate=44100.0, channels=1, block_size=BLOCK)
    assert lms.attach_master_output(master, out_stream=0, channels=1, sample_rate=SR, block_size=BLOCK)
    a_be.set_input_value(0.3)
    b_be.set_input_value(0.1)

    path = str(tmp_path / "offclock.wav")
    # Ring large enough to hold the whole burst (this test ticks far faster than real time).
    assert lms.attach_wav_recorder(path, ring_frames=4000 * BLOCK + BLOCK)
    for be in (a_be, b_be, master):
        be.start()
    acc = 0.0
    pumped = 0
    for _ in range(4000):
        master.tick(BLOCK)
        pumped += BLOCK
        a_be.tick(BLOCK)
        acc += BLOCK * (44100.0 / 48000.0)   # feed source B at its own (slower) clock
        while acc >= BLOCK:
            b_be.tick(BLOCK)
            acc -= BLOCK
    lms.stop_recording()
    # Conservation: every pumped frame is either written to disk or dropped (RT-safe overflow).
    assert lms.recorded_frames + lms.record_dropped_frames == pumped
    assert lms.recorded_frames == pumped and lms.record_dropped_frames == 0  # ring held it all
    r = aud.WavReader(path)
    assert r.ok and r.total_frames == lms.recorded_frames


@pytest.mark.live
def test_live_mic_to_wav(aud):
    """LiveMultiSource on real hardware: a live mic (its own clock) + speakers as the master
    clock, recording the processed signal to a WAV. Live-gated (AIUDIO_LIVE=1)."""
    if not hasattr(aud, "InputBackend"):
        return
    import os
    import tempfile

    g = aud.Graph()
    src, gn, k = g.add_source(0), g.add_gain(0.5), g.add_sink(0)
    g.connect(src, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=512)
    lms = aud.LiveMultiSource(ex, engine_rate=SR, channels=1, max_block=512)

    mic, spk = aud.InputBackend(), aud.DeviceBackend()
    assert lms.attach_source(mic, stream=0, source_rate=SR, channels=1, block_size=512)
    assert lms.attach_master_output(spk, out_stream=0, channels=1, sample_rate=SR, block_size=512)

    path = os.path.join(tempfile.mkdtemp(), "mic.wav")
    assert lms.attach_wav_recorder(path)
    assert mic.start()
    assert spk.start()
    try:
        time.sleep(0.5)          # <-- recording duration
    finally:
        lms.stop_recording()
        spk.stop()
        mic.stop()
    assert lms.recorded_frames > 0
    assert aud.WavReader(path).total_frames > 0

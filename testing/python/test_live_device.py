"""Live RT device I/O, driven from Python (G7 / ADR-0010) — the LIVENESS layer.

Gated by `@pytest.mark.live` (auto-skipped unless AIUDIO_LIVE=1 on a macOS box with an
output device; see conftest.py). Live output is SILENT today (no signal-generator node),
so these assert the *frontend contract* via telemetry + behavior, not audio content:
the stream starts, the C++ audio thread runs the graph at the right cadence, live
control is accepted while it runs, the GIL is released (Python stays responsive), and
stop is clean. Python never touches the audio thread.

    AIUDIO_LIVE=1 pytest testing/python/test_live_device.py -v
"""
from __future__ import annotations

import time

import pytest

from _graphs import BLOCK, SR

pytestmark = pytest.mark.live

EXPECTED_RATE = SR / BLOCK  # blocks/sec the IOProc should call process() at


def _opened(aud):
    g = aud.Graph()
    s = g.add_source()
    gn = g.add_gain(0.5)
    lp = g.add_biquad_lowpass(2000.0, 0.707, SR)
    m = g.add_meter()
    k = g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, lp, 0)
    g.connect(lp, 0, m, 0)
    g.connect(m, 0, k, 0)
    assert g.validate()[0]
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=2, sample_rate=SR, max_block=BLOCK)
    be = aud.DeviceBackend()
    assert be.open(ex, channels=2, sample_rate=SR, block_size=BLOCK), "open() failed"
    return be, ex, gn, lp, m


def test_enumerate_lists_an_output(aud):
    devices = aud.DeviceBackend().enumerate()
    assert isinstance(devices, list) and devices
    assert any(d.output_channels > 0 for d in devices)


def test_audio_thread_runs_at_expected_cadence(aud):
    be, ex, *_ = _opened(aud)
    assert not be.running
    assert be.start()
    try:
        assert be.running                       # lifecycle telemetry
        time.sleep(0.5)
        rc1 = ex.render_count
        assert rc1 > 0                           # the C++ IOProc actually ran the graph
        time.sleep(0.5)
        rate = (ex.render_count - rc1) / 0.5
        assert abs(rate - EXPECTED_RATE) < 0.3 * EXPECTED_RATE, f"{rate:.0f}/s vs {EXPECTED_RATE:.0f}/s"
    finally:
        be.stop()
    assert not be.running
    frozen = ex.render_count
    time.sleep(0.2)
    assert ex.render_count == frozen             # stopped: no more blocks


def test_live_control_accepted_while_running(aud):
    be, ex, gn, lp, _ = _opened(aud)
    assert be.start()
    try:
        time.sleep(0.2)
        for hz in (400.0, 1200.0, 6000.0):
            assert ex.set_cutoff(lp, hz)         # enqueued (False only if the queue is full)
            assert ex.set_gain(gn, 0.3)
            assert ex.set_q(lp, 0.707)
            time.sleep(0.05)
        assert ex.render_count > 0
    finally:
        be.stop()


def test_gil_released_python_runs_concurrently(aud):
    # If start() held the GIL, this Python loop couldn't advance while audio ran.
    be, ex, *_ = _opened(aud)
    assert be.start()
    try:
        ticks, t0 = 0, time.monotonic()
        while time.monotonic() - t0 < 0.5:
            time.sleep(0.02)
            _ = ex.render_count
            ticks += 1
        assert ticks > 10 and ex.render_count > 0
    finally:
        be.stop()


def test_restart_same_backend(aud):
    be, ex, *_ = _opened(aud)
    assert be.start()
    time.sleep(0.2)
    be.stop()
    after_first = ex.render_count
    assert be.start()                            # stop() keeps the IOProc, so restart works
    time.sleep(0.2)
    be.stop()
    assert ex.render_count > after_first


def test_latency_reported(aud):
    be, *_ = _opened(aud)
    assert be.latency_frames >= 0

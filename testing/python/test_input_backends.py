"""Input / duplex / process-tap backends bound to Python (M11a).

Enumeration and process listing are permission-free and headless-safe (run on the macOS
CI leg). Actually *capturing* (open+start) needs microphone TCC permission, so that is a
gated `live` test that skips cleanly if it can't open a device.
"""
from __future__ import annotations

import sys
import time

import pytest

_MAC = sys.platform == "darwin"


@pytest.mark.skipif(not _MAC, reason="input/duplex/tap backends are macOS-only")
def test_backends_exported(aud):
    for name in ("InputBackend", "DuplexBackend", "TapBackend", "ProcessInfo"):
        assert hasattr(aud, name), f"missing aiudio.{name}"
    assert {"InputBackend", "DuplexBackend", "TapBackend"}.issubset(set(aud.__all__))


@pytest.mark.skipif(not _MAC, reason="macOS-only")
def test_input_enumerate_lists_an_input(aud):
    devices = aud.InputBackend().enumerate()
    assert isinstance(devices, list) and devices
    assert any(d.input_channels > 0 for d in devices)  # at least the built-in mic


@pytest.mark.skipif(not _MAC, reason="macOS-only")
def test_tap_list_processes_permission_free(aud):
    procs = aud.TapBackend.list_processes()  # no permission needed
    assert isinstance(procs, list) and procs
    for p in procs[:5]:
        assert isinstance(p.pid, int)
        assert isinstance(p.bundle_id, str)


@pytest.mark.live
def test_input_capture_runs(aud):
    if not hasattr(aud, "InputBackend"):
        pytest.skip("InputBackend not built")
    g = aud.Graph()
    s, m, k = g.add_source(), g.add_meter(), g.add_sink()
    g.connect(s, 0, m, 0)
    g.connect(m, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=48000.0, max_block=512)

    be = aud.InputBackend()
    if not be.open(ex, channels=1, sample_rate=48000.0, block_size=512):
        pytest.skip("could not open an input device")
    assert be.start()
    try:
        assert be.running
        time.sleep(0.5)
        assert ex.render_count > 0   # the C++ input IOProc captured + ran the graph
    finally:
        be.stop()
    assert not be.running

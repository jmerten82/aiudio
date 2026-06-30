"""Live path, headless (M10 master-clock adapter + M9.4 mock backend).

A MockBackend's clock drives the MasterClockAdapter, which feeds the MultiSourceManager +
graph and returns the result to the device — the full live pipeline with no hardware. Also
exercises hot-unplug (clean + surfaced) and device-side xruns.
"""
from __future__ import annotations

SR = 48000.0


def _gain_graph(aud, gain):
    g = aud.Graph()
    s, gn, k = g.add_source(0), g.add_gain(gain), g.add_sink(0)
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)
    return ex


def test_live_path_flows(aud):
    ex = _gain_graph(aud, 0.5)
    mgr = aud.MultiSourceManager(1, 1, 1, 64, 256)
    adapter = aud.MasterClockAdapter(mgr, ex, in_stream=0, out_stream=0)
    dev = aud.MockBackend()
    assert dev.open(adapter, in_channels=1, out_channels=1, block_size=64)
    dev.set_input_value(0.5)
    assert dev.start()
    dev.tick(32)
    assert abs(dev.captured_output(0) - 0.25) < 1e-6   # in 0.5 → gain 0.5 → out 0.25


def test_hot_unplug_is_clean_and_recovers(aud):
    ex = _gain_graph(aud, 1.0)
    mgr = aud.MultiSourceManager(1, 1, 1, 64, 256)
    adapter = aud.MasterClockAdapter(mgr, ex, 0, 0)
    dev = aud.MockBackend()
    assert dev.open(adapter, 1, 1, 64)
    dev.set_input_value(1.0)
    dev.start()
    dev.tick(32)
    assert abs(dev.captured_output(0) - 1.0) < 1e-6

    dev.inject_disconnect()
    assert dev.disconnected and not dev.running
    dev.tick(32)  # no-op, no crash

    dev.inject_reconnect()
    assert dev.start()
    dev.set_input_value(0.25)
    dev.tick(32)
    assert abs(dev.captured_output(0) - 0.25) < 1e-6   # recovered


def test_device_xruns_counted(aud):
    dev = aud.MockBackend()
    assert dev.xrun_count == 0
    dev.inject_xrun(64)
    dev.inject_xrun(32)
    assert dev.xrun_count == 96


def test_real_device_open_accepts_adapter(aud):
    """The device backends' open() accepts a MasterClockAdapter (not just an executor), so a
    real Core Audio device can drive the MultiSourceManager. macOS-only; needs no hardware to
    *start* — open() returns a bool either way, proving the overload is bound (no TypeError)."""
    import pytest

    if not hasattr(aud, "DeviceBackend"):
        pytest.skip("Core Audio device backends are macOS-only")
    ex = _gain_graph(aud, 0.5)
    mgr = aud.MultiSourceManager(1, 1, 1, 64, 256)
    adapter = aud.MasterClockAdapter(mgr, ex, in_stream=0, out_stream=0)
    # output / input / duplex: open() now accepts a MasterClockAdapter. (TapBackend's overload
    # exists too but needs a configured tap target + entitlements, so it's left to a live run.)
    for cls in (aud.DeviceBackend, aud.InputBackend, aud.DuplexBackend):
        opened = cls().open(adapter, block_size=64)   # the MasterClockAdapter open overload
        assert isinstance(opened, bool)               # dispatched (False if no such device present)

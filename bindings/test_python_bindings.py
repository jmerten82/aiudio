"""Tests for the aiudio Python bindings (G6 / M8).

Run via the build dir, e.g.:
    PYTHONPATH=build-py/bindings python bindings/test_python_bindings.py
or with pytest once installed (`pip install .`): `pytest`.
"""

from __future__ import annotations

import numpy as np

try:
    import aiudio as _a  # packaged install
except ModuleNotFoundError:
    import _aiudio as _a  # dev: extension built directly by CMake


def test_source_gain_sink_numpy() -> None:
    g = _a.Graph()
    src = g.add_source()
    gain = g.add_gain(0.5)
    sink = g.add_sink()
    assert g.connect(src, 0, gain, 0)
    assert g.connect(gain, 0, sink, 0)
    ok, err = g.validate()
    assert ok, err

    ex = _a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=48000.0, max_block=512)
    assert ex.compiled

    x = np.ones((1, 256), dtype=np.float32)
    y = ex.process(x)
    assert y.shape == (1, 256)
    assert np.allclose(y, 0.5), y[0, :4]


def test_validate_rejects_cycle() -> None:
    g = _a.Graph()
    a0 = g.add_gain(1.0)
    b0 = g.add_gain(1.0)
    g.connect(a0, 0, b0, 0)
    g.connect(b0, 0, a0, 0)  # cycle
    ok, _err = g.validate()
    assert not ok


def test_live_param_edit_via_queue() -> None:
    # The RT-safe control path: enqueue a parameter change on the executor; it is
    # applied at the top of the next block. This is the call you'd use while a device
    # backend is running — Python never touches the audio thread.
    g = _a.Graph()
    src = g.add_source()
    gain = g.add_gain(0.5)
    sink = g.add_sink()
    g.connect(src, 0, gain, 0)
    g.connect(gain, 0, sink, 0)
    ex = _a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=48000.0, max_block=128)

    x = np.ones((1, 128), dtype=np.float32)
    assert np.allclose(ex.process(x), 0.5)
    assert ex.set_gain(gain, 0.25)  # queue (returns False only if the queue is full)
    assert np.allclose(ex.process(x), 0.25)  # applied on the next block
    assert ex.render_count >= 2  # telemetry: blocks processed


def test_live_cutoff_via_queue() -> None:
    sr = 48000.0
    g = _a.Graph()
    s = g.add_source()
    lp = g.add_biquad_lowpass(500.0, 0.707, sr)  # tight LP attenuates a 6 kHz tone
    k = g.add_sink()
    g.connect(s, 0, lp, 0)
    g.connect(lp, 0, k, 0)
    ex = _a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=sr, max_block=256)

    t = np.arange(256) / sr
    tone = np.sin(2 * np.pi * 6000 * t).astype(np.float32).reshape(1, 256)
    for _ in range(8):
        lo = ex.process(tone)
    assert ex.set_cutoff(lp, 18000.0)  # open the filter live
    for _ in range(8):
        hi = ex.process(tone)
    assert np.sqrt((hi**2).mean()) > 3 * np.sqrt((lo**2).mean())


def test_device_backend_enumerate() -> None:
    # macOS-only control frontend. Enumeration is headless-safe (no stream started); we
    # never start() in tests (that needs a device and would make sound).
    if not hasattr(_a, "DeviceBackend"):
        return  # not built on this platform
    be = _a.DeviceBackend()
    devices = be.enumerate()
    assert isinstance(devices, list)
    assert not be.running
    for d in devices:
        assert isinstance(d.name, str)
        assert d.output_channels >= 0


if __name__ == "__main__":
    test_source_gain_sink_numpy()
    test_validate_rejects_cycle()
    test_live_param_edit_via_queue()
    test_live_cutoff_via_queue()
    test_device_backend_enumerate()
    print("all python binding tests passed")

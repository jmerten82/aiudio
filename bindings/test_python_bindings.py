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


def test_live_param_edit() -> None:
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
    g.set_gain(gain, 0.25)  # edit the live node's parameter
    assert np.allclose(ex.process(x), 0.25)


if __name__ == "__main__":
    test_source_gain_sink_numpy()
    test_validate_rejects_cycle()
    test_live_param_edit()
    print("all python binding tests passed")

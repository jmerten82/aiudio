"""Packaging / public-API surface — `pip install .` must yield a usable `aiudio`.

Guards against the kind of regression we already hit once: a symbol existing in the
compiled `_aiudio` module but not re-exported from the `aiudio` package.
"""
from __future__ import annotations

import sys

import pytest

CORE = ["Graph", "GraphExecutor", "OfflineBackend", "WavFormat"]


def test_core_symbols_exported(aud):
    for name in CORE:
        assert hasattr(aud, name), f"missing aiudio.{name}"
    assert set(CORE).issubset(set(aud.__all__))


def test_version_present(aud):
    assert isinstance(aud.__version__, str) and aud.__version__


def test_wavformat_values(aud):
    assert aud.WavFormat.Int16 is not None
    assert aud.WavFormat.Float32 is not None


@pytest.mark.skipif(sys.platform != "darwin", reason="DeviceBackend is macOS-only")
def test_device_symbols_present_on_macos(aud):
    assert hasattr(aud, "DeviceBackend")
    assert hasattr(aud, "AudioDeviceInfo")
    assert {"DeviceBackend", "AudioDeviceInfo"}.issubset(set(aud.__all__))


@pytest.mark.skipif(sys.platform == "darwin", reason="checking the non-macOS guard")
def test_device_symbols_absent_off_macos(aud):
    # On non-macOS the package must still import; device symbols simply aren't there.
    assert not hasattr(aud, "DeviceBackend")

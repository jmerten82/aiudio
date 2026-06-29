"""Shared fixtures + marker gating for the Phase-0 Python test suites.

- `aud` — the imported `aiudio` package (skips the whole module if it isn't built).
- `@pytest.mark.live` tests are auto-skipped unless `AIUDIO_LIVE=1` *and* a
  `DeviceBackend` is present (i.e. you're on a macOS box with an output device).
"""
from __future__ import annotations

import os

import pytest


@pytest.fixture(scope="session")
def aud():
    return pytest.importorskip("aiudio")


def pytest_collection_modifyitems(config, items):
    live_enabled = os.environ.get("AIUDIO_LIVE") == "1"
    try:
        import aiudio

        has_device = hasattr(aiudio, "DeviceBackend")
    except Exception:
        has_device = False

    skip_live = pytest.mark.skip(
        reason="live RT device test — set AIUDIO_LIVE=1 on a macOS box with an output device"
    )
    for item in items:
        if "live" in item.keywords and not (live_enabled and has_device):
            item.add_marker(skip_live)

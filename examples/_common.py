"""Shared helpers for the M0 audio I/O spikes.

These spikes exist only to prove permissions + device plumbing end-to-end before
any C++ is written (see ``docs/71-io-layer-milestones.md`` M0). ``numpy`` is the
only import needed for ``--self-test``; ``sounddevice`` is imported lazily, so the
offline self-tests run even without PortAudio installed.
"""

from __future__ import annotations

import argparse
from typing import Any

DEFAULT_SAMPLERATE = 48_000
DEFAULT_BLOCKSIZE = 128  # frames per callback — matches the framework's RT target


def add_common_args(parser: argparse.ArgumentParser) -> None:
    """Add the I/O arguments shared by every spike."""
    parser.add_argument(
        "--samplerate", type=int, default=DEFAULT_SAMPLERATE, help="Hz (default 48000)"
    )
    parser.add_argument(
        "--blocksize",
        type=int,
        default=DEFAULT_BLOCKSIZE,
        help="frames per callback (default 128)",
    )
    parser.add_argument(
        "--duration", type=float, default=5.0, help="seconds to run (default 5)"
    )
    parser.add_argument(
        "--device",
        default=None,
        help="device index, or a substring of its name (e.g. 'Kanto', 'Sennheiser')",
    )
    parser.add_argument(
        "--list-devices",
        action="store_true",
        help="print the available audio devices and exit",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the offline DSP self-test (no audio device, no permissions) and exit",
    )


def list_devices() -> None:
    """Print all audio devices Core Audio exposes (no stream opened)."""
    import sounddevice as sd  # lazy: only needed for real device work

    print(sd.query_devices())
    default_in, default_out = sd.default.device
    print(f"\nDefault input  : {default_in}")
    print(f"Default output : {default_out}")


def resolve_device(spec: str | None, kind: str) -> int | None:
    """Resolve a device spec (index or name substring) to a device index.

    ``kind`` is ``"input"`` or ``"output"``; ``None`` spec returns ``None``
    (sounddevice then uses the system default).
    """
    if spec is None:
        return None
    import sounddevice as sd

    if spec.isdigit():
        return int(spec)
    needle = spec.lower()
    channels_key = "max_input_channels" if kind == "input" else "max_output_channels"
    for idx, dev in enumerate(sd.query_devices()):
        info: dict[str, Any] = dict(dev)
        if needle in str(info["name"]).lower() and info[channels_key] > 0:
            return idx
    raise SystemExit(f"No {kind} device matching {spec!r}. Try --list-devices.")


def dbfs(rms: float) -> float:
    """Convert a linear RMS value to dBFS (full-scale = 0 dB)."""
    import numpy as np

    return float(20.0 * np.log10(max(rms, 1e-9)))


def meter_bar(level_dbfs: float, width: int = 40) -> str:
    """Render a simple text level meter for a dBFS value in [-60, 0]."""
    frac = max(0.0, min(1.0, (level_dbfs + 60.0) / 60.0))
    filled = int(frac * width)
    return "[" + "#" * filled + "-" * (width - filled) + f"] {level_dbfs:6.1f} dBFS"

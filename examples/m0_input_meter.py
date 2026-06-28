#!/usr/bin/env python3
"""M0 spike (2/3): capture an input device and show a live level meter.

Proves the *input* plumbing and the microphone TCC permission flow. Acceptance
(docs/71 M0): see the meter respond to the chosen input (e.g. the Sennheiser).

    python examples/m0_input_meter.py --list-devices
    python examples/m0_input_meter.py --device Sennheiser
    python examples/m0_input_meter.py --self-test     # offline, no mic, no permission
"""

from __future__ import annotations

import argparse

import numpy as np

from _common import add_common_args, dbfs, list_devices, meter_bar, resolve_device


def block_rms(block: np.ndarray) -> float:
    """RMS of a block of samples (any channel count)."""
    return float(np.sqrt(np.mean(block.astype(np.float64) ** 2)))


def run(args: argparse.Namespace) -> None:
    import sounddevice as sd  # lazy: real device work only

    device = resolve_device(args.device, "input")

    def callback(indata: np.ndarray, frames: int, _time, status) -> None:
        if status:
            print(f"\n[status] {status}")
        print("\r" + meter_bar(dbfs(block_rms(indata))), end="", flush=True)

    print(f"Metering input for {args.duration}s "
          f"@ {args.samplerate} Hz / {args.blocksize} frames. Ctrl-C to stop.")
    with sd.InputStream(
        samplerate=args.samplerate,
        blocksize=args.blocksize,
        device=device,
        channels=1,
        dtype="float32",
        callback=callback,
    ):
        sd.sleep(int(args.duration * 1000))
    print()


def self_test() -> int:
    """Verify the meter offline against signals of known level."""
    sr = 48_000
    # Full-scale sine → RMS = 1/sqrt(2) ≈ -3.01 dBFS
    full = np.sin(2.0 * np.pi * 1000.0 * np.arange(sr) / sr).astype(np.float32)
    # Half-amplitude sine → ~ -9.03 dBFS
    half = (0.5 * full).astype(np.float32)
    silence = np.zeros(sr, dtype=np.float32)

    d_full, d_half, d_sil = dbfs(block_rms(full)), dbfs(block_rms(half)), dbfs(block_rms(silence))
    ok_full = abs(d_full - (-3.01)) < 0.1
    ok_half = abs(d_half - (-9.03)) < 0.1
    ok_sil = d_sil < -100.0
    print(f"self-test: full-scale={d_full:6.2f} dBFS (exp -3.01) {'OK' if ok_full else 'FAIL'}")
    print(f"self-test: half-scale={d_half:6.2f} dBFS (exp -9.03) {'OK' if ok_half else 'FAIL'}")
    print(f"self-test: silence  ={d_sil:6.1f} dBFS (exp << 0)  {'OK' if ok_sil else 'FAIL'}")
    return 0 if (ok_full and ok_half and ok_sil) else 1


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    args = parser.parse_args()

    if args.list_devices:
        list_devices()
    elif args.self_test:
        raise SystemExit(self_test())
    else:
        run(args)


if __name__ == "__main__":
    main()

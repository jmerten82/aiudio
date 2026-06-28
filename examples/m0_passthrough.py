#!/usr/bin/env python3
"""M0 spike (3/3): full-duplex passthrough — input straight to output.

Proves the *duplex* path on one clock (input + output in a single callback) — the
precursor to real-time effects. Acceptance (docs/71 M0): talk into the input and
hear yourself from the output with no audible dropouts at 48 kHz / 128 frames.

    python examples/m0_passthrough.py --device-in Sennheiser --device-out Kanto
    python examples/m0_passthrough.py --self-test     # offline, no audio device

WARNING: use headphones — open speakers + an open mic will feed back.
"""

from __future__ import annotations

import argparse

import numpy as np

from _common import add_common_args, list_devices, resolve_device


def passthrough(indata: np.ndarray, outdata: np.ndarray, gain: float) -> None:
    """Copy input to output with gain, matching channel counts as best we can."""
    n = min(indata.shape[1], outdata.shape[1])
    outdata[:, :n] = indata[:, :n] * gain
    if outdata.shape[1] > n:  # mono in, stereo out → duplicate
        outdata[:, n:] = outdata[:, :1] * gain
    if indata.shape[1] > n:  # extra input channels ignored
        pass


def run(args: argparse.Namespace) -> None:
    import sounddevice as sd  # lazy: real device work only

    dev_in = resolve_device(args.device_in, "input")
    dev_out = resolve_device(args.device_out, "output")
    xruns = {"count": 0}

    def callback(indata: np.ndarray, outdata: np.ndarray, frames: int, _time, status) -> None:
        if status:
            xruns["count"] += 1
            print(f"\n[status] {status}")
        passthrough(indata, outdata, args.gain)

    print(f"Passthrough for {args.duration}s @ {args.samplerate} Hz / "
          f"{args.blocksize} frames. Use headphones! Ctrl-C to stop.")
    with sd.Stream(
        samplerate=args.samplerate,
        blocksize=args.blocksize,
        device=(dev_in, dev_out),
        channels=(1, 2),
        dtype="float32",
        callback=callback,
    ):
        sd.sleep(int(args.duration * 1000))
    print(f"\nDone. xrun/status events: {xruns['count']}")


def self_test() -> int:
    """Verify the passthrough copies input to output correctly, offline."""
    rng = np.random.default_rng(0)
    block = rng.standard_normal((128, 1)).astype(np.float32)

    # mono in → stereo out, unity gain: both output channels equal the input
    out_stereo = np.zeros((128, 2), dtype=np.float32)
    passthrough(block, out_stereo, gain=1.0)
    ok_copy = np.allclose(out_stereo[:, 0], block[:, 0])
    ok_dup = np.allclose(out_stereo[:, 1], block[:, 0])

    # gain applied
    out_gain = np.zeros((128, 1), dtype=np.float32)
    passthrough(block, out_gain, gain=0.5)
    ok_gain = np.allclose(out_gain[:, 0], block[:, 0] * 0.5, atol=1e-6)

    print(f"self-test: copy L {'OK' if ok_copy else 'FAIL'} | "
          f"duplicate R {'OK' if ok_dup else 'FAIL'} | "
          f"gain {'OK' if ok_gain else 'FAIL'}")
    return 0 if (ok_copy and ok_dup and ok_gain) else 1


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    parser.add_argument("--device-in", default=None, help="input device index or name")
    parser.add_argument("--device-out", default=None, help="output device index or name")
    parser.add_argument("--gain", type=float, default=1.0, help="linear output gain")
    args = parser.parse_args()

    if args.list_devices:
        list_devices()
    elif args.self_test:
        raise SystemExit(self_test())
    else:
        run(args)


if __name__ == "__main__":
    main()

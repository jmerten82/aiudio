#!/usr/bin/env python3
"""M0 spike (1/3): play a phase-continuous sine to an output device.

Proves the *output* plumbing. Acceptance (docs/pipeline/71 M0): hear a clean 440 Hz tone
from the chosen output (e.g. the Kanto ORA4).

    python examples/m0_sine_out.py --list-devices
    python examples/m0_sine_out.py --device Kanto --freq 440
    python examples/m0_sine_out.py --self-test      # offline, no audio device
"""

from __future__ import annotations

import argparse

import numpy as np

from _common import add_common_args, list_devices, resolve_device


class SineGenerator:
    """Stateful, phase-continuous sine source (no per-block discontinuities)."""

    def __init__(self, freq: float, samplerate: int, amplitude: float = 0.2) -> None:
        self.freq = freq
        self.samplerate = samplerate
        self.amplitude = amplitude
        self._phase = 0.0

    def render(self, frames: int) -> np.ndarray:
        """Return ``frames`` mono float32 samples, advancing the phase."""
        t = (np.arange(frames, dtype=np.float64) + self._phase) / self.samplerate
        block = (self.amplitude * np.sin(2.0 * np.pi * self.freq * t)).astype(np.float32)
        self._phase = (self._phase + frames) % self.samplerate
        return block


def run(args: argparse.Namespace) -> None:
    import sounddevice as sd  # lazy: real device work only

    device = resolve_device(args.device, "output")
    gen = SineGenerator(args.freq, args.samplerate)

    def callback(outdata: np.ndarray, frames: int, _time, status) -> None:
        if status:
            print(f"\n[status] {status}")
        outdata[:, 0] = gen.render(frames)  # mono into channel 0
        if outdata.shape[1] > 1:
            outdata[:, 1] = outdata[:, 0]  # duplicate to right

    print(f"Playing {args.freq} Hz for {args.duration}s "
          f"@ {args.samplerate} Hz / {args.blocksize} frames. Ctrl-C to stop.")
    with sd.OutputStream(
        samplerate=args.samplerate,
        blocksize=args.blocksize,
        device=device,
        channels=2,
        dtype="float32",
        callback=callback,
    ):
        sd.sleep(int(args.duration * 1000))


def self_test() -> int:
    """Verify the generator offline: RMS, dominant frequency, block continuity."""
    sr, freq, amp = 48_000, 440.0, 0.2
    gen = SineGenerator(freq, sr, amplitude=amp)
    blocks = [gen.render(128) for _ in range(sr // 128)]  # ~1 s, block by block
    sig = np.concatenate(blocks)

    rms = float(np.sqrt(np.mean(sig.astype(np.float64) ** 2)))
    expected_rms = amp / np.sqrt(2.0)
    spectrum = np.abs(np.fft.rfft(sig * np.hanning(len(sig))))
    peak_hz = float(np.fft.rfftfreq(len(sig), 1.0 / sr)[int(np.argmax(spectrum))])
    # Phase continuity: no jump at block boundaries — adjacent samples never differ
    # by more than the theoretical per-sample max for this frequency/amplitude.
    max_step = float(np.max(np.abs(np.diff(sig))))
    step_bound = amp * 2.0 * np.pi * freq / sr * 1.5

    ok_rms = abs(rms - expected_rms) < 1e-3
    ok_freq = abs(peak_hz - freq) < 2.0
    ok_cont = np.isfinite(sig).all() and max_step < step_bound
    print(f"self-test: RMS={rms:.4f} (exp {expected_rms:.4f}) {'OK' if ok_rms else 'FAIL'}")
    print(f"self-test: peak={peak_hz:.1f} Hz (exp {freq}) {'OK' if ok_freq else 'FAIL'}")
    print(f"self-test: max step={max_step:.5f} (< {step_bound:.5f}) "
          f"{'OK' if ok_cont else 'FAIL'}")
    return 0 if (ok_rms and ok_freq and ok_cont) else 1


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    add_common_args(parser)
    parser.add_argument("--freq", type=float, default=440.0, help="tone frequency (Hz)")
    args = parser.parse_args()

    if args.list_devices:
        list_devices()
    elif args.self_test:
        raise SystemExit(self_test())
    else:
        run(args)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Example: drive a LIVE Core Audio device from Python — as a control frontend (G7).

This is the point of the control plane: Python opens/starts/stops a real-time audio
stream and **controls + observes** it, while the audio thread runs entirely in C++.
Python NEVER runs on the audio thread (ADR-0004):

  * lifecycle  — open()/start()/stop() are off-thread C calls (they release the GIL);
  * control    — ex.set_gain()/set_cutoff() enqueue onto a lock-free queue the audio
                 thread drains at the top of each block (RT-safe);
  * telemetry  — ex.render_count and g.meter_mean_square() are atomics the audio
                 thread publishes and Python polls.

Note: there is no signal-generator node yet, and an output device hands the graph an
empty input, so this renders **silence**. That's fine — the goal here is to prove the
*frontend*: render_count climbing means the C++ audio thread is running your graph;
set_gain/set_cutoff are applied lock-free; Python only ever observes and commands.
(For audible processing today, route a file through the graph with OfflineBackend, or
the mic with the duplex backend once that's bound.)

    python examples/python/ex_live_control.py --list-devices
    python examples/python/ex_live_control.py --seconds 3
    python examples/python/ex_live_control.py --device "MacBook Pro Speakers"

Needs a real output device (run on the Mac, not in CI).
"""

from __future__ import annotations

import argparse
import time

try:
    import aiudio
except ModuleNotFoundError:  # dev: extension built directly by CMake
    import _aiudio as aiudio


def list_devices() -> None:
    for d in aiudio.DeviceBackend().enumerate():
        tag = "".join(["I" if d.is_default_input else "", "O" if d.is_default_output else ""])
        print(f"  [{tag:>2}] {d.name!r}  in={d.input_channels} out={d.output_channels}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list-devices", action="store_true", help="list audio devices and exit")
    ap.add_argument("--device", default="", help="output device name substring (default: system default)")
    ap.add_argument("--seconds", type=float, default=3.0, help="how long to run the live stream")
    ap.add_argument("--sample-rate", type=float, default=48000.0)
    ap.add_argument("--block", type=int, default=512)
    args = ap.parse_args()

    if not hasattr(aiudio, "DeviceBackend"):
        raise SystemExit("DeviceBackend is macOS-only and isn't present in this build.")
    if args.list_devices:
        list_devices()
        return

    # Resolve the output device (substring match on the enumerated names).
    backend = aiudio.DeviceBackend()
    device_id = ""
    for d in backend.enumerate():
        if args.device and args.device.lower() in d.name.lower() and d.output_channels > 0:
            device_id = d.id
            print(f"using output device: {d.name!r}")
            break

    # Build  Source -> Gain -> Biquad(lowpass) -> Meter -> Sink, and compile it.
    g = aiudio.Graph()
    src = g.add_source()
    gain = g.add_gain(0.8)
    lp = g.add_biquad_lowpass(2000.0, 0.707, args.sample_rate)
    meter = g.add_meter()
    sink = g.add_sink()
    g.connect(src, 0, gain, 0)
    g.connect(gain, 0, lp, 0)
    g.connect(lp, 0, meter, 0)
    g.connect(meter, 0, sink, 0)
    ok, err = g.validate()
    assert ok, err

    ex = aiudio.GraphExecutor()
    assert ex.compile(g, channels=2, sample_rate=args.sample_rate, max_block=args.block), "compile failed"

    # --- Open + start the device. After start() returns, the C++ IOProc thread is
    #     calling ex.process() on its own; Python is free to observe and command. ---
    assert backend.open(ex, channels=2, sample_rate=args.sample_rate, block_size=args.block,
                        output_device=device_id), "failed to open device"
    backend.start()
    print(f"stream running={backend.running}  latency={backend.latency_frames} frames\n")

    # Poll telemetry and push live control edits from this (control) thread.
    t_end = time.monotonic() + args.seconds
    last = 0
    step = 0
    while time.monotonic() < t_end:
        time.sleep(0.25)
        blocks = ex.render_count            # telemetry (atomic, audio thread publishes)
        level = g.meter_mean_square(meter)  # telemetry (atomic)
        print(f"  blocks={blocks:>6} (+{blocks - last:>4})  meter_ms={level:.2e}")
        last = blocks
        # Sweep gain and filter cutoff live — applied at the next audio block, lock-free.
        step += 1
        ex.set_gain(gain, 0.8 if step % 2 else 0.3)
        ex.set_cutoff(lp, 500.0 + 300.0 * step)

    backend.stop()
    print(f"\nstopped. total blocks processed by the C++ audio thread: {ex.render_count}")
    print("Python issued every gain/cutoff change without ever touching the audio thread.")


if __name__ == "__main__":
    main()

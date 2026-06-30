#!/usr/bin/env python3
"""Example: capture a LIVE input device from Python (M11a).

The input/duplex/tap Core Audio backends are now bound (previously only output was).
This drives a microphone through a graph and reads the level off-thread — Python opens/
starts/stops and observes; the capture IOProc is pure C++ (ADR-0004). Capturing the mic
needs **microphone TCC permission** (granted to your terminal/Python on first run).

    python examples/python/ex_live_input.py --list-devices
    python examples/python/ex_live_input.py --seconds 3
    python examples/python/ex_live_input.py --device "MacBook Pro Microphone"

Sibling backends bound the same way:
  * DuplexBackend  — mic → graph → speakers on one clock (live monitoring).
  * TapBackend     — system / per-app OUTPUT capture (needs a signed binary + TCC).
"""
from __future__ import annotations

import argparse
import math
import time

try:
    import aiudio
except ModuleNotFoundError:
    import _aiudio as aiudio


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list-devices", action="store_true")
    ap.add_argument("--device", default="", help="input device name substring (default: system default)")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--sample-rate", type=float, default=48000.0)
    ap.add_argument("--block", type=int, default=512)
    args = ap.parse_args()

    if not hasattr(aiudio, "InputBackend"):
        raise SystemExit("InputBackend is macOS-only and isn't present in this build.")

    backend = aiudio.InputBackend()
    if args.list_devices:
        for d in backend.enumerate():
            if d.input_channels > 0:
                tag = "default-in" if d.is_default_input else ""
                print(f"  {d.name!r:42} in={d.input_channels} {tag}")
        return

    device_id = ""
    for d in backend.enumerate():
        if args.device and args.device.lower() in d.name.lower() and d.input_channels > 0:
            device_id = d.id
            print(f"using input device: {d.name!r}")
            break

    # mic -> Source -> Meter -> Sink (input-only; the graph's `out` is empty)
    g = aiudio.Graph()
    src = g.add_source()
    meter = g.add_meter()
    sink = g.add_sink()
    g.connect(src, 0, meter, 0)
    g.connect(meter, 0, sink, 0)
    ex = aiudio.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=args.sample_rate, max_block=args.block), "compile failed"

    if not backend.open(ex, channels=1, sample_rate=args.sample_rate, block_size=args.block,
                        input_device=device_id):
        raise SystemExit("could not open the input device")
    backend.start()
    print(f"capturing: running={backend.running}  latency={backend.latency_frames} frames "
          f"(grant mic permission if the level stays at -inf)\n")

    t_end = time.monotonic() + args.seconds
    while time.monotonic() < t_end:
        time.sleep(0.25)
        ms = g.meter_mean_square(meter)   # telemetry (atomic; published by the audio thread)
        dbfs = 10 * math.log10(ms) if ms > 0 else float("-inf")
        bar = "#" * max(0, int((dbfs + 60) / 3))
        print(f"  blocks={ex.render_count:>6}  level={dbfs:6.1f} dBFS  {bar}")

    backend.stop()
    print(f"\nstopped. {ex.render_count} blocks captured by the C++ input thread; "
          "Python only observed (never touched the audio thread).")


if __name__ == "__main__":
    main()

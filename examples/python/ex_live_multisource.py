#!/usr/bin/env python3
"""Example: N live sources on DIFFERENT clocks → one graph → a master output (LiveMultiSource).

Headless + deterministic: MockBackends stand in for the source + master device clocks, so it
runs anywhere. Source B is on a 44.1 kHz clock while the engine/master run at 48 kHz — the
per-source drift servo brings it onto the engine timeline. See the commented block for the
real-hardware swap (a live mic + speakers).

    python examples/python/ex_live_multisource.py
"""
from __future__ import annotations

try:
    import aiudio as a
except ModuleNotFoundError:
    import _aiudio as a

SR = 48000.0


def main() -> None:
    # Graph: source 0 (synth) + source 1 (noise) → mixer → sink 0 (→ master output device).
    g = a.Graph()
    s0, s1 = g.add_source(0), g.add_source(1)
    mx, out = g.add_mixer(2, 1.0), g.add_sink(0)
    g.connect(s0, 0, mx, 0)
    g.connect(s1, 0, mx, 1)
    g.connect(mx, 0, out, 0)
    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=256)
    ex.set_param(mx, 1, 0.4)  # source 1 quieter

    lms = a.LiveMultiSource(ex, engine_rate=SR, channels=1, max_block=256)
    src_synth, src_noise, master = a.MockBackend(), a.MockBackend(), a.MockBackend()
    lms.attach_source(src_synth, stream=0, source_rate=SR, block_size=256, ring_frames=8192)
    lms.attach_source(src_noise, stream=1, source_rate=44100.0, block_size=256, ring_frames=8192)  # 44.1k clock
    lms.attach_master_output(master, out_stream=0, sample_rate=SR, block_size=256)

    # In a headless run we feed the mock sources by setting their capture value + ticking; on real
    # hardware each backend's IOProc drives itself (see below). Different tick cadences = different clocks.
    src_synth.set_input_value(0.5)
    src_noise.set_input_value(0.3)
    src_synth.start()
    src_noise.start()
    master.start()

    acc = 0.0
    for _ in range(4000):
        src_synth.tick(256)
        acc += 256 * (44100.0 / SR)
        while acc >= 256:
            src_noise.tick(256)
            acc -= 256
        master.tick(256)

    print(f"{lms.source_count} live sources → graph → master output")
    print(f"  master out sample : {master.captured_output(0):.3f}  (0.5 + 0.3*0.4 = 0.62)")
    print(f"  source 1 (44.1k)  : ratio={lms.source_ratio(1):.5f} (→44100/48000≈0.919) "
          f"fill={lms.source_fill(1)} overruns={lms.source_overruns(1)}")

    # --- Real hardware (macOS): swap the mocks for live backends; each IOProc drives itself. ---
    #   mic, spk = a.InputBackend(), a.DeviceBackend()
    #   lms.attach_source(mic, stream=0, source_rate=SR, channels=1)      # mic on its own clock
    #   lms.attach_master_output(spk, out_stream=0, channels=2, sample_rate=SR)  # speakers = master clock
    #   mic.start(); spk.start()   # ... plays the live mix ... then: spk.stop(); mic.stop()


if __name__ == "__main__":
    main()

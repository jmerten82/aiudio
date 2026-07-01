#!/usr/bin/env python3
"""Example: two live sources → per-source gain → sum → **recorded to a WAV** for a duration.

This is the "mic + system audio → gain each → mix → out.wav" recorder. The audio thread taps
the mixed master output into a lock-free ring; a writer thread drains it to disk (ADR-0004 —
file I/O never touches the audio thread). `DURATION_S` bounds the recording.

Headless + deterministic by default: MockBackends stand in for the two sources + the master
clock, so it runs anywhere and writes a real WAV. The commented block shows the real-hardware
swap: a live mic + a system-audio loopback (BlackHole) as sources, speakers as the master clock.

    python examples/python/ex_record_multisource.py
"""
from __future__ import annotations

import tempfile
from pathlib import Path

try:
    import aiudio as a
except ModuleNotFoundError:
    import _aiudio as a

SR = 48000.0
BLOCK = 256
DURATION_S = 2.0          # recording length (parameter)
MIC_GAIN, SYS_GAIN = 1.0, 0.6  # per-source gains (parameter)


def build() -> tuple[a.GraphExecutor, int, int]:
    # source 0 (mic) → gain → sum:0 ; source 1 (system) → gain → sum:1 ; sum → sink(0)
    g = a.Graph()
    mic, mic_g = g.add_source(0), g.add_gain(MIC_GAIN)
    sys_, sys_g = g.add_source(1), g.add_gain(SYS_GAIN)
    mix, out = g.add_sum(2), g.add_sink(0)
    g.connect(mic, 0, mic_g, 0)
    g.connect(mic_g, 0, mix, 0)
    g.connect(sys_, 0, sys_g, 0)
    g.connect(sys_g, 0, mix, 1)
    g.connect(mix, 0, out, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    return ex, mic_g, sys_g


def main() -> None:
    ex, _mic_g, _sys_g = build()
    lms = a.LiveMultiSource(ex, engine_rate=SR, channels=1, max_block=BLOCK)
    out_path = Path(tempfile.gettempdir()) / "aiudio_multisource_recording.wav"

    # --- Headless mock version (runs anywhere) ---------------------------------------------
    mic, sysrc, master = a.MockBackend(), a.MockBackend(), a.MockBackend()
    lms.attach_source(mic, stream=0, source_rate=SR, channels=1, block_size=BLOCK)
    lms.attach_source(sysrc, stream=1, source_rate=44100.0, channels=1, block_size=BLOCK)  # off-clock
    lms.attach_master_output(master, out_stream=0, channels=1, sample_rate=SR, block_size=BLOCK)
    mic.set_input_value(0.4)      # stand-in "mic"
    sysrc.set_input_value(0.2)    # stand-in "system audio"

    # This mock loop ticks far faster than real time, so size the ring to hold the whole run
    # (on real hardware the writer thread keeps up in real time and a ~1 s ring suffices).
    total_frames = (int(DURATION_S * SR) // BLOCK) * BLOCK
    lms.attach_wav_recorder(str(out_path), format=a.WavFormat.Float32, ring_frames=total_frames + BLOCK)
    for be in (mic, sysrc, master):
        be.start()
    acc = 0.0
    for _ in range(int(DURATION_S * SR) // BLOCK):   # advance the mock clocks for the duration
        master.tick(BLOCK)
        mic.tick(BLOCK)
        acc += BLOCK * (44100.0 / 48000.0)
        while acc >= BLOCK:
            sysrc.tick(BLOCK)
            acc -= BLOCK
    lms.stop_recording()
    for be in (master, sysrc, mic):
        be.stop()

    # --- Real hardware (mic + system-audio loopback → speakers as master) ------------------
    # mic, sysrc = a.InputBackend(), a.InputBackend()   # system audio via a BlackHole loopback
    # master = a.DeviceBackend()
    # lms.attach_source(mic,   stream=0, source_rate=SR, channels=1, input_device="")         # default mic
    # lms.attach_source(sysrc, stream=1, source_rate=SR, channels=1, input_device="BlackHole2ch_UID")
    # lms.attach_master_output(master, out_stream=0, channels=1, sample_rate=SR)
    # lms.attach_wav_recorder(str(out_path))
    # mic.start(); sysrc.start(); master.start()
    # import time; time.sleep(DURATION_S)              # <-- duration
    # lms.stop_recording(); master.stop(); sysrc.stop(); mic.stop()

    r = a.WavReader(str(out_path))
    print(f"wrote {out_path}")
    print(f"  frames={lms.recorded_frames} dropped={lms.record_dropped_frames}")
    peak = float(abs(r.read(r.total_frames)).max())
    print(f"  peak {peak:.3f}  (mock mix: 0.4*{MIC_GAIN} + 0.2*{SYS_GAIN} = "
          f"{0.4 * MIC_GAIN + 0.2 * SYS_GAIN:.3f})")


if __name__ == "__main__":
    main()

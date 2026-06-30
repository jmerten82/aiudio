#!/usr/bin/env python3
"""Example: the multi-source manager (M10) — N sources + M sinks on one clock.

This is the milestone where it composes: the `MultiSourceManager` owns a lock-free ring
per (stream, channel); producers push audio into input streams, the pump runs the
multi-stream graph (G10) once per block, and consumers pop from output streams. Mixing /
routing is the graph's job (a `SumNode` + per-stream gains here).

Here Python plays all the roles (feeds 2 sources, pumps, reads 2 outputs) so it runs
anywhere with no device. The live wiring — a real mic/tap feeding an input ring on the
master clock — is the remaining thin layer (and, for genuinely off-clock devices, needs
the drift compensation of M9.5).

    PYTHONPATH=build-py/bindings python examples/python/ex_multisource.py
    # or, after `pip install .`:  python examples/python/ex_multisource.py
"""
from __future__ import annotations

import numpy as np

try:
    import aiudio as a
except ModuleNotFoundError:
    import _aiudio as a

SR = 48000.0
BLOCK = 256


def main() -> None:
    # 2 inputs (e.g. mic + backing track) -> per-source gain -> mixed -> 2 outputs
    # (mains + a quieter monitor send).
    g = a.Graph()
    in_a, in_b = g.add_source(stream=0), g.add_source(stream=1)
    gain_a, gain_b = g.add_gain(0.8), g.add_gain(0.5)
    mix = g.add_sum(2)
    mains = g.add_sink(stream=0)
    monitor_gain = g.add_gain(0.3)
    monitor = g.add_sink(stream=1)
    g.connect(in_a, 0, gain_a, 0)
    g.connect(in_b, 0, gain_b, 0)
    g.connect(gain_a, 0, mix, 0)
    g.connect(gain_b, 0, mix, 1)
    g.connect(mix, 0, mains, 0)
    g.connect(mix, 0, monitor_gain, 0)
    g.connect(monitor_gain, 0, monitor, 0)
    ok, err = g.validate()
    assert ok, err

    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    mgr = a.MultiSourceManager(num_inputs=2, num_outputs=2, channels=1, max_block=BLOCK,
                               ring_frames=4 * BLOCK)
    print(f"manager: {mgr.num_inputs} sources -> graph -> {mgr.num_outputs} sinks "
          f"({mgr.channels} ch); executor streams: {ex.input_streams} in / {ex.output_streams} out")

    # Two distinct input signals: 220 Hz and 440 Hz tones, streamed block by block.
    n = 4 * BLOCK
    t = np.arange(n) / SR
    sig_a = (0.5 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
    sig_b = (0.5 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)

    mains_acc = monitor_acc = 0.0
    blocks = n // BLOCK
    for i in range(blocks):
        sl = slice(i * BLOCK, (i + 1) * BLOCK)
        mgr.push_input(0, sig_a[sl].reshape(1, BLOCK))   # producer A
        mgr.push_input(1, sig_b[sl].reshape(1, BLOCK))   # producer B
        mgr.process(ex, BLOCK)                            # the pump (one master clock tick)
        mains_acc += float((mgr.pop_output(0, BLOCK) ** 2).mean())     # consumer: mains
        monitor_acc += float((mgr.pop_output(1, BLOCK) ** 2).mean())   # consumer: monitor

    mains_rms = (mains_acc / blocks) ** 0.5
    monitor_rms = (monitor_acc / blocks) ** 0.5
    print(f"mains   RMS = {mains_rms:.4f}  (0.8·A + 0.5·B)")
    print(f"monitor RMS = {monitor_rms:.4f}  (mains · 0.3)")
    print(f"monitor/mains = {monitor_rms / mains_rms:.3f}  (expect ~0.30)")
    print(f"telemetry — input underruns: {mgr.input_underruns(0)}, {mgr.input_underruns(1)}; "
          f"output overruns: {mgr.output_overruns(0)}, {mgr.output_overruns(1)}")
    print("OK")


if __name__ == "__main__":
    main()

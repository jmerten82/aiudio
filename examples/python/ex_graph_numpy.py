#!/usr/bin/env python3
"""Example: build and run an aiudio graph from Python, with numpy I/O (G6 / M8).

The graph engine + nodes are C++; Python builds the graph and pushes/pulls audio
as numpy arrays. This is the bridge the ML layer (Phase 1) and the agent (Phase 2)
will use.

    PYTHONPATH=build-py/bindings python examples/python/ex_graph_numpy.py
    # or, after `pip install .`:  python examples/python/ex_graph_numpy.py
"""

from __future__ import annotations

import numpy as np

try:
    import aiudio as a
except ModuleNotFoundError:
    import _aiudio as a


def main() -> None:
    sr = 48000.0
    # Build  Source -> Biquad(lowpass 1 kHz) -> Gain(0.8) -> Meter -> Sink
    g = a.Graph()
    src = g.add_source()
    lp = g.add_biquad_lowpass(1000.0, 0.707, sr)
    gain = g.add_gain(0.8)
    meter = g.add_meter()
    sink = g.add_sink()
    g.connect(src, 0, lp, 0)
    g.connect(lp, 0, gain, 0)
    g.connect(gain, 0, meter, 0)
    g.connect(meter, 0, sink, 0)
    ok, err = g.validate()
    print(f"graph: {g.node_count} nodes, validate={ok} {err}")

    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=sr, max_block=1024), "compile failed"

    # Push a 300 Hz + 5 kHz mix through, block by block, as numpy.
    block = 512
    t0 = 0
    in_rms_acc = out_rms_acc = 0.0
    n_blocks = 90  # ~1 s
    for _ in range(n_blocks):
        t = (t0 + np.arange(block)) / sr
        x = (0.4 * np.sin(2 * np.pi * 300 * t) + 0.4 * np.sin(2 * np.pi * 5000 * t)).astype(np.float32)
        x = x.reshape(1, block)            # (channels, frames), planar
        y = ex.process(x)                  # -> numpy (1, block)
        in_rms_acc += float(np.mean(x**2))
        out_rms_acc += float(np.mean(y**2))
        t0 += block

    in_rms = (in_rms_acc / n_blocks) ** 0.5
    out_rms = (out_rms_acc / n_blocks) ** 0.5
    print(f"in RMS  = {in_rms:.3f} (300 Hz + 5 kHz)")
    print(f"out RMS = {out_rms:.3f} (after LP 1 kHz + gain 0.8)")
    print(f"meter mean-square (last block) = {g.meter_mean_square(meter):.5f}")

    # Live parameter edit from Python.
    g.set_gain(gain, 0.1)
    y = ex.process(np.ones((1, block), dtype=np.float32))
    print(f"after set_gain(0.1): DC-ish out level ~ {float(np.mean(np.abs(y))):.3f}")

    print("OK")


if __name__ == "__main__":
    main()

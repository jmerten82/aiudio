#!/usr/bin/env python3
"""Example: multiple input AND output streams through one graph (G10).

The multi-stream executor lets a single graph receive N independent input streams and
drive M output streams — each SourceNode/SinkNode binds to a stream index, and
`process_multi([...])` passes one numpy array per input stream and returns one per
output stream. This is the graph-side foundation for true multi-source I/O (docs/pipeline/76):
the live multi-source manager (M10) will feed these streams from N device rings; here
we feed them from numpy.

    PYTHONPATH=build-py/bindings python examples/python/ex_multistream.py
    # or, after `pip install .`:  python examples/python/ex_multistream.py
"""
from __future__ import annotations

import numpy as np

try:
    import aiudio as a
except ModuleNotFoundError:
    import _aiudio as a

SR = 48000.0


def main() -> None:
    # Two input streams (mic + backing track, say) -> per-source gain -> mixed -> two
    # output streams (mains + a monitor send at a different level).
    g = a.Graph()
    in_a = g.add_source(stream=0)   # input stream 0
    in_b = g.add_source(stream=1)   # input stream 1
    gain_a = g.add_gain(0.8)
    gain_b = g.add_gain(0.5)
    mix = g.add_sum(2)
    mains = g.add_sink(stream=0)    # output stream 0
    monitor_gain = g.add_gain(0.3)
    monitor = g.add_sink(stream=1)  # output stream 1 (a quieter monitor mix)

    g.connect(in_a, 0, gain_a, 0)
    g.connect(in_b, 0, gain_b, 0)
    g.connect(gain_a, 0, mix, 0)
    g.connect(gain_b, 0, mix, 1)
    g.connect(mix, 0, mains, 0)          # mix -> mains
    g.connect(mix, 0, monitor_gain, 0)   # mix -> monitor (attenuated)
    g.connect(monitor_gain, 0, monitor, 0)

    ok, err = g.validate()
    print(f"graph: {g.node_count} nodes, validate={ok} {err}")

    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=512), "compile failed"
    print(f"input_streams={ex.input_streams}  output_streams={ex.output_streams}  channels={ex.channels}")

    # Two distinct input signals: a 220 Hz tone and a 440 Hz tone.
    n = 512
    t = np.arange(n) / SR
    sig_a = (0.5 * np.sin(2 * np.pi * 220 * t)).astype(np.float32).reshape(1, n)
    sig_b = (0.5 * np.sin(2 * np.pi * 440 * t)).astype(np.float32).reshape(1, n)

    outs = ex.process_multi([sig_a, sig_b])   # -> [mains, monitor]
    mains_rms = float(np.sqrt((outs[0] ** 2).mean()))
    monitor_rms = float(np.sqrt((outs[1] ** 2).mean()))
    print(f"mains   RMS = {mains_rms:.4f}  (gain_a·A + gain_b·B)")
    print(f"monitor RMS = {monitor_rms:.4f}  (mains · 0.3)")
    print(f"monitor/mains ratio = {monitor_rms / mains_rms:.3f}  (expect ~0.30)")

    # The same graph still runs through the single-stream path (back-compatible):
    one = ex.process(sig_a)   # stream-0 in -> stream-0 out (mains)
    print(f"single-stream process still works: out shape {one.shape}")
    print("OK")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Example: WAV read/write from Python (numpy) — offline/tooling I/O (M6).

Renders a short tone through a gain graph and writes the processed output to a WAV, then
reads it back. This is the OFFLINE/control-thread path (blocking file I/O — never the audio
thread, ADR-0004); it is also the non-RT foundation for a live recorder (drain a lock-free
ring into a WavWriter on a writer thread).

    python examples/python/ex_wav_io.py
"""
from __future__ import annotations

import tempfile
from pathlib import Path

import numpy as np

try:
    import aiudio as a
except ModuleNotFoundError:
    import _aiudio as a

SR = 48000.0


def main() -> None:
    # A trivial graph: source → gain(0.5) → sink.
    g = a.Graph()
    src, gn, k = g.add_source(), g.add_gain(0.5), g.add_sink()
    g.connect(src, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=512)

    out_path = Path(tempfile.gettempdir()) / "aiudio_ex_wav_io.wav"
    frames = 512
    n0 = 0
    # `with` finalizes the WAV header deterministically on exit.
    with a.WavWriter(str(out_path), channels=1, sample_rate=SR,
                     format=a.WavFormat.Float32) as w:
        for _ in range(20):  # ~0.2 s of a 440 Hz tone, processed block by block
            n = np.arange(n0, n0 + frames)
            tone = (0.8 * np.sin(2 * np.pi * 440.0 * n / SR)).astype(np.float32)[None, :]
            processed = ex.process(tone)          # C++ runs the graph
            w.write(np.ascontiguousarray(processed))  # Python writes off-thread
            n0 += frames

    r = a.WavReader(str(out_path))
    print(f"wrote {out_path}")
    print(f"  ok={r.ok} channels={r.channels} sample_rate={r.sample_rate} "
          f"frames={r.total_frames}")
    peak = float(np.max(np.abs(r.read(r.total_frames))))
    print(f"  read back peak amplitude {peak:.3f} (≈ 0.8 * gain 0.5 = 0.4)")


if __name__ == "__main__":
    main()

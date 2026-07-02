#!/usr/bin/env python3
"""Example: a neural node as a first-class graph peer (Phase 1 · D7).

DSP and neural nodes are peers in one graph (the aiudio thesis). Here a tiny learned nonlinearity
(a torch nn.Module) sits between a DSP gain and the output, and **both** train jointly to match a
target — then the trained module exports (TorchScript) toward real-time deployment (ADR-0006).

    pip install "aiudio[diff]"
    python examples/python/ex_diff_neural_node.py
"""
from __future__ import annotations

try:
    import aiudio as a
    import aiudio.diff as adiff
    import torch
    import torch.nn as nn
except ModuleNotFoundError as exc:
    raise SystemExit(f'this example needs the diff layer: pip install "aiudio[diff]"  ({exc})')

SR, BLOCK = 48000.0, 512


class TinyAmp(nn.Module):
    """A tiny learned per-sample nonlinearity (NAM-flavored)."""

    def __init__(self, hidden=16):
        super().__init__()
        self.net = nn.Sequential(nn.Linear(1, hidden), nn.Tanh(), nn.Linear(hidden, 1))

    def forward(self, x):
        b, c, n = x.shape
        return self.net(x.reshape(-1, 1)).reshape(b, c, n)


def main() -> None:
    # source → gain (DSP) → neural (learned) → sink
    g = a.Graph()
    s, gn, nid, k = g.add_source(), g.add_gain(1.0), g.add_neural_node(), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, nid, 0)
    g.connect(nid, 0, k, 0)
    a.GraphExecutor().compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    print("neural node config (C++ side):", dict(g.node_config(nid)), "— RT is Phase 3 (ADR-0006)")

    model = TinyAmp()
    de = adiff.DiffExecutor(g, modules={nid: model})     # inject the module for the neural slot

    # target: a saturation the gain + net should jointly learn to reproduce
    torch.manual_seed(0)
    x = 1.5 * torch.randn(8, 1, BLOCK)
    target = torch.tanh(2.5 * x)
    hist = adiff.fit(de, adiff.mse, lambda: (x, target), steps=400, lr=0.02, seed=0)
    print(f"joint DSP+neural training: loss {hist[0]:.4f} -> {hist[-1]:.4f}  "
          f"(render match {adiff.max_abs_diff(de(x), target):.3e})")

    # deploy path (ADR-0006, Phase 3): export the trained module as a portable graph via
    # torch.export (→ ONNX / ExecuTorch / LibTorch) — the modern replacement for TorchScript.
    exported = torch.export.export(model.eval(), (x,))
    print("trained module exported for deployment:",
          torch.allclose(exported.module()(x), model(x), atol=1e-6))
    # (the DSP gain also trained; push it to the C++ graph as usual — export_to_graph.)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Example: match a target render by gradient descent (Phase 1 · D5).

The headline of the differentiable core: given a target produced by a graph with *unknown*
parameters, recover them by backprop through the (audio-domain, multi-node) differentiable graph —
then hand the learned parameters to the C++ real-time graph (D6).

Here: source → gain → waveshaper(tanh) → sink. We render a target with secret params, then fit a
fresh graph (default init) to reproduce it.

    pip install "aiudio[diff]"
    python examples/python/ex_diff_param_match.py
"""
from __future__ import annotations

import numpy as np

try:
    import aiudio as a
    import aiudio.diff as adiff
    import torch
except ModuleNotFoundError as exc:
    raise SystemExit(f'this example needs the diff layer: pip install "aiudio[diff]"  ({exc})')

SR, BLOCK = 48000.0, 512


def main() -> None:
    g = a.Graph()
    s, gn, ws, k = g.add_source(), g.add_gain(1.0), g.add_waveshaper("tanh", 1.0, 1.0), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, ws, 0)
    g.connect(ws, 0, k, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    # A "secret" target: render the graph with unknown params (the thing we want to recover).
    secret = {gn: {0: 0.8}, ws: {0: 3.0, 1: 0.6}}
    rng = np.random.default_rng(0)
    x = torch.from_numpy((1.5 * rng.standard_normal((4, 1, BLOCK))).astype(np.float32))
    target = adiff.DiffExecutor(g, init_params=secret)(x).detach()

    # Recover it from a default init by gradient descent (multi-resolution STFT loss).
    learner = adiff.DiffExecutor(g)
    hist = adiff.match_target(learner, x, target, loss_fn=adiff.MultiResolutionSTFTLoss(),
                              steps=600, lr=0.03, seed=0)
    p = dict(learner.named_parameters())
    print(f"loss: {hist[0]:.3e} -> {hist[-1]:.3e}  (render match {adiff.max_abs_diff(learner(x), target):.2e})")
    print("recovered vs secret:")
    print(f"  gain  {p[f'_diff.{gn}.gain'].item():.3f}  (0.800)")
    print(f"  drive {p[f'_diff.{ws}.drive'].item():.3f}  (3.000)")
    print(f"  mix   {p[f'_diff.{ws}.mix'].item():.3f}  (0.600)")

    # D6 preview: push the learned params into the C++ real-time graph.
    ex.set_gain(gn, p[f"_diff.{gn}.gain"].item())
    ex.set_param(ws, 0, p[f"_diff.{ws}.drive"].item())
    ex.set_param(ws, 1, p[f"_diff.{ws}.mix"].item())
    print("learned params written into the C++ graph (ex.set_param) — ready for real time.")


if __name__ == "__main__":
    main()

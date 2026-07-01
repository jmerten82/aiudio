#!/usr/bin/env python3
"""Example: the differentiable executor (Phase 1 · D0, ADR-0016/0017).

Builds the SAME `Graph` the C++ engine runs, wraps it in a `DiffExecutor` (Python/PyTorch), shows
that the torch forward matches the C++ `process()` (parity), and then takes a few gradient steps to
recover an unknown gain from a target render — a tiny preview of the D5 "match a target" slice.

Requires the optional differentiable layer:  pip install "aiudio[diff]"

    python examples/python/ex_diff_executor.py
"""
from __future__ import annotations

import numpy as np

try:
    import aiudio as a
    import aiudio.diff as adiff
    import torch
except ModuleNotFoundError as exc:
    raise SystemExit(f'this example needs the diff layer: pip install "aiudio[diff]"  ({exc})')

SR, BLOCK = 48000.0, 256


def main() -> None:
    # source → gain → sink, built once; the C++ executor and the DiffExecutor both read it.
    g = a.Graph()
    src, gain, snk = g.add_source(), g.add_gain(0.5), g.add_sink()
    g.connect(src, 0, gain, 0)
    g.connect(gain, 0, snk, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    # 1) Parity: the torch forward matches the C++ process().
    de = adiff.DiffExecutor(g, init_params={gain: {0: 0.5}})
    x = np.full((1, BLOCK), 0.8, np.float32)
    print(f"C++/torch parity max|Δ| = {adiff.assert_parity(ex, de, x):.2e}")

    # 2) Recover an unknown gain by gradient descent (preview of D5).
    target_gain = 0.30
    rng = np.random.default_rng(0)
    xb = torch.from_numpy(rng.standard_normal((8, 1, BLOCK)).astype(np.float32))
    target = xb * target_gain

    learner = adiff.DiffExecutor(g, init_params={gain: {0: 1.0}})  # start wrong (1.0)
    opt = torch.optim.Adam(learner.parameters(), lr=0.05)
    g_param = dict(learner.named_parameters())[f"_diff.{gain}.gain"]
    for step in range(200):
        opt.zero_grad()
        loss = (learner(xb) - target).pow(2).mean()
        loss.backward()
        opt.step()
        if step % 50 == 0 or step == 199:
            print(f"  step {step:3d}: gain={g_param.detach().item():.4f}  loss={loss.item():.2e}")
    print(f"recovered gain {g_param.detach().item():.4f} (target {target_gain}) — write it back with "
          f"ex.set_gain(node, value) for real-time (D6).")


if __name__ == "__main__":
    main()

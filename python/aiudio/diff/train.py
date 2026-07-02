"""A minimal, reproducible training harness for the differentiable core (Phase 1 · D4).

`fit` runs the standard loop — data → forward → loss → backward → optimizer step — over a
`DiffExecutor` (or any ``nn.Module``), returning the loss history. `seed_everything` makes runs
deterministic; `save_checkpoint`/`load_checkpoint` round-trip the trained parameters (which then
export into the C++ graph — D6). Deliberately thin: autograd + a torch optimizer do the work.
"""
from __future__ import annotations

import random
from collections.abc import Callable

import numpy as np
import torch


def seed_everything(seed: int = 0) -> None:
    """Seed Python, NumPy, and torch RNGs for reproducible training."""
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)


def fit(model: torch.nn.Module, loss_fn: Callable[[torch.Tensor, torch.Tensor], torch.Tensor],
        batch_fn: Callable[[], tuple[torch.Tensor, torch.Tensor]], *, steps: int = 200,
        lr: float = 0.05, optimizer_cls: type[torch.optim.Optimizer] = torch.optim.Adam,
        seed: int | None = None) -> list[float]:
    """Optimize ``model``'s parameters to minimize ``loss_fn(model(x), target)``.

    ``batch_fn()`` returns an ``(input, target)`` pair each step (use ``lambda: (x, target)`` for a
    fixed target, or a closure that draws fresh batches). Returns the per-step loss history. If
    ``seed`` is given, the run is deterministic.
    """
    if seed is not None:
        seed_everything(seed)
    optimizer = optimizer_cls(model.parameters(), lr=lr)
    history: list[float] = []
    for _ in range(steps):
        x, target = batch_fn()
        optimizer.zero_grad()
        loss = loss_fn(model(x), target)
        loss.backward()
        optimizer.step()
        history.append(float(loss.detach()))
    return history


def match_target(model: torch.nn.Module, x: torch.Tensor, target: torch.Tensor, *,
                 loss_fn: Callable[[torch.Tensor, torch.Tensor], torch.Tensor] | None = None,
                 steps: int = 300, lr: float = 0.05, seed: int | None = None) -> list[float]:
    """Recover ``model``'s parameters so ``model(x)`` matches a fixed ``target`` render, by
    gradient descent (Phase 1 · D5 — the "match a target / brighten the vocal" slice). ``loss_fn``
    defaults to MSE; pass `MultiResolutionSTFTLoss()` for a spectral objective. Returns the loss
    history. The recovered params then export into the C++ graph for real time (D6)."""
    from .losses import mse
    return fit(model, loss_fn or mse, lambda: (x, target), steps=steps, lr=lr, seed=seed)


def export_to_graph(diff_executor, executor) -> int:
    """Write a trained `DiffExecutor`'s parameters into the compiled C++ `GraphExecutor` via
    `set_param` — the ML-first → real-time round-trip (Phase 1 · D6). Returns the number of params
    written. `set_param` is queued, so run a `process()` block afterward to drain + apply it (any
    smoothed params — waveshaper/mixer/pan/delay — settle over a few blocks; atomic params —
    gain/compressor/gate — apply on the first block)."""
    written = 0
    for node_id, params in diff_executor.export_params().items():
        for index, value in params.items():
            if executor.set_param(node_id, index, value):
                written += 1
    return written


def save_checkpoint(model: torch.nn.Module, path: str) -> None:
    """Save the model's trained parameters (state_dict) to ``path``."""
    torch.save(model.state_dict(), path)


def load_checkpoint(model: torch.nn.Module, path: str) -> None:
    """Restore parameters saved by :func:`save_checkpoint` into ``model`` (in place)."""
    model.load_state_dict(torch.load(path, weights_only=True))

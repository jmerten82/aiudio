"""The C++↔torch parity harness (ADR-0016).

The "one IR" guarantee across the third executor: a node's differentiable ``forward()`` must match
its C++ ``process()`` numerically. ``assert_parity`` runs the *same* input through the compiled C++
``GraphExecutor`` and a `DiffExecutor` and checks they agree within tolerance — the gate every
differentiable node passes in CI.
"""
from __future__ import annotations

import numpy as np
import torch


def max_abs_diff(a, b) -> float:
    """Max absolute elementwise difference between two arrays/tensors of equal shape."""
    an = a.detach().cpu().numpy() if isinstance(a, torch.Tensor) else np.asarray(a)
    bn = b.detach().cpu().numpy() if isinstance(b, torch.Tensor) else np.asarray(b)
    return float(np.max(np.abs(an - bn)))


def assert_parity(cpp_executor, diff_executor, x_np, *, warmup: int = 4, tol: float = 1e-5) -> float:
    """Assert the C++ executor and the DiffExecutor agree on the same ``(channels, frames)`` input.

    The C++ side is warmed up (``warmup`` blocks) so any per-parameter smoothing — a click-free RT
    detail absent from the diff forward — has settled, making the comparison a steady-state one.
    Returns the measured max abs difference.
    """
    x_np = np.ascontiguousarray(x_np, dtype=np.float32)
    for _ in range(max(0, warmup)):
        cpp_executor.process(x_np)
    cpp_out = cpp_executor.process(x_np)  # settled block

    xt = torch.from_numpy(x_np).unsqueeze(0)  # (channels, frames) -> [1, channels, frames]
    diff_out = diff_executor(xt).squeeze(0)   # -> (channels, frames)

    d = max_abs_diff(cpp_out, diff_out)
    if d > tol:
        raise AssertionError(f"C++/torch parity mismatch: max|Δ| = {d:.3e} > tol {tol:.1e}")
    return d

"""aiudio.diff — the differentiable / trainable layer (Phase 1, ADR-0016/0017).

A **third executor** (Python/PyTorch, off-thread) that interprets the *same* aiudio `Graph` IR
through autograd, so DSP-node parameters are trainable and (later) neural models are first-class
peers. The real-time core stays C++ and is never touched (ADR-0004); this layer is research/ML
only — heavy, batched, GPU/MPS-friendly.

**Optional** — requires PyTorch. Install with:  ``pip install "aiudio[diff]"``. torch is imported
lazily here so the base ``aiudio`` package (the RT core + control frontend) needs no torch.

D0 (this milestone): the executor spine + node registry + the C++↔torch parity harness, with the
trivial nodes (Source/Sink/Gain/Sum). Subsequent milestones add the rest of the node library
(D1–D3), losses + a trainer (D4), the parameter-match slice (D5), and the round-trip to RT (D6).
See ``docs/79`` for the full plan.
"""
from __future__ import annotations

try:
    import torch as _torch  # noqa: F401
except ModuleNotFoundError as exc:  # pragma: no cover - exercised only without torch
    raise ModuleNotFoundError(
        'aiudio.diff requires PyTorch (the differentiable layer, ADR-0017). '
        'Install it with:  pip install "aiudio[diff]"'
    ) from exc

from .executor import DiffExecutor
from .filters import DiffBiquad, FilterType, fit_magnitude
from .losses import MultiResolutionSTFTLoss, l1, mse
from .nodes import (
    DiffNode,
    Differentiability,
    make_diff_node,
    register_diff_node,
    registered_types,
)
from .parity import assert_parity, max_abs_diff
from .train import fit, load_checkpoint, match_target, save_checkpoint, seed_everything

__all__ = [
    "DiffExecutor",
    "DiffNode",
    "Differentiability",
    "register_diff_node",
    "make_diff_node",
    "registered_types",
    "assert_parity",
    "max_abs_diff",
    "DiffBiquad",
    "FilterType",
    "fit_magnitude",
    "MultiResolutionSTFTLoss",
    "mse",
    "l1",
    "fit",
    "match_target",
    "seed_everything",
    "save_checkpoint",
    "load_checkpoint",
]

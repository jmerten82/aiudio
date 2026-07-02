"""aiudio.diff — the differentiable / trainable layer (Phase 1, ADR-0016/0017).

A **third executor** (Python/PyTorch, off-thread) that interprets the *same* aiudio `Graph` IR
through autograd, so DSP-node parameters are trainable and (later) neural models are first-class
peers. The real-time core stays C++ and is never touched (ADR-0004); this layer is research/ML
only — heavy, batched, GPU/MPS-friendly.

**Optional** — requires PyTorch. Install with:  ``pip install "aiudio[diff]"``. torch is imported
lazily here so the base ``aiudio`` package (the RT core + control frontend) needs no torch.

Phase 1 is **complete** (D0–D8, see ``docs/79``): the executor spine + node registry + C++↔torch
parity harness (D0); the full DSP node library — linear (D1), filters (D2), dynamics/nonlinear/
recursive (D3); losses + a trainer (D4); the parameter-match slice (D5); the round-trip to RT (D6);
the first neural node (D7); and a DDSP synth exemplar (D8, `HarmonicSynth`).
"""
from __future__ import annotations

try:
    import torch as _torch  # noqa: F401
except ModuleNotFoundError as exc:  # pragma: no cover - exercised only without torch
    raise ModuleNotFoundError(
        'aiudio.diff requires PyTorch (the differentiable layer, ADR-0017). '
        'Install it with:  pip install "aiudio[diff]"'
    ) from exc

from .ddsp import HarmonicSynth
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
from .train import (
    export_to_graph,
    fit,
    load_checkpoint,
    match_target,
    save_checkpoint,
    seed_everything,
)

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
    "HarmonicSynth",
    "MultiResolutionSTFTLoss",
    "mse",
    "l1",
    "fit",
    "match_target",
    "export_to_graph",
    "seed_everything",
    "save_checkpoint",
    "load_checkpoint",
]

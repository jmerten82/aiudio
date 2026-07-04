"""Differentiable parameter tuning for the workbench (Phase 2 · C2, ADR-0016/0022).

The "params by gradient" half of *render → measure → self-correct*: take the current graph, run it
through the Phase-1 differentiable executor, optimize its parameters against a target render
(`match_target`, D5), and write the tuned values back into the session (as `set_param` actions, so
they're logged, broadcast, and shown in the UI — the D6 write-back). The LLM chooses *structure*
(C0/C1); this tunes the *numbers*.

Requires the differentiable extra (`pip install "aiudio[diff]"`); torch is imported lazily, so the
base workbench stays torch-free. Raises ``ModuleNotFoundError`` if the extra is absent.
"""
from __future__ import annotations

from typing import TYPE_CHECKING

import aiudio

if TYPE_CHECKING:
    from .session import GraphSession


def tune_to_target(session: "GraphSession", x, target, *, loss_fn=None, steps: int = 300,
                   lr: float = 0.05, seed: int = 0, sample_rate: float = 48000.0) -> dict:
    """Tune ``session``'s graph parameters so its render of ``x`` matches ``target``.

    ``x``/``target`` are array-likes shaped ``[channels, frames]`` or ``[batch, channels, frames]``.
    Returns ``{loss_before, loss_after, steps}``; the tuned parameters are applied to the session
    (persisted + broadcastable). ``loss_fn`` defaults to the multi-resolution STFT loss.
    """
    import numpy as np
    import torch

    import aiudio.diff as adiff

    xt = torch.as_tensor(np.asarray(x), dtype=torch.float32)
    tt = torch.as_tensor(np.asarray(target), dtype=torch.float32)
    if xt.ndim == 2:
        xt = xt.unsqueeze(0)
    if tt.ndim == 2:
        tt = tt.unsqueeze(0)
    channels, frames = xt.shape[-2], xt.shape[-1]

    # Compile once so param_value reflects real (settled) params, then mirror + start from any
    # already-tuned overrides recorded in the document (idempotent / iterative tuning).
    executor = aiudio.GraphExecutor()
    if not executor.compile(session.graph, channels=channels, sample_rate=sample_rate,
                            max_block=int(frames)):
        raise ValueError("graph did not compile — cannot tune")
    doc = session.to_document()
    init_params = {n["id"]: {int(k): v for k, v in n["params"].items()}
                   for n in doc["nodes"] if n["params"]}

    diff = adiff.DiffExecutor(session.graph, init_params=init_params, sample_rate=sample_rate)
    loss = loss_fn if loss_fn is not None else adiff.MultiResolutionSTFTLoss()
    history = adiff.match_target(diff, xt, tt, loss_fn=loss, steps=steps, lr=lr, seed=seed)

    # write the tuned parameters back into the session (D6 write-back → logged + broadcast)
    for node_id, params in diff.export_params().items():
        for index, value in params.items():
            session.set_param(node_id, index, value)

    return {"loss_before": history[0], "loss_after": history[-1], "steps": steps}

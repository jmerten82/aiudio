"""C2 — differentiable parameter tuning in the workbench (Phase 2, ADR-0016/0022).

The "params by gradient" loop: tune the current graph to match a target render (Phase-1
match_target), writing the tuned params back into the session. Gated on the differentiable extra.
"""
from __future__ import annotations

import numpy as np
import pytest

pytest.importorskip("torch")

from aiudio import workbench as wb  # noqa: E402

SR = 48000.0


def _gain_session(initial: float) -> tuple[wb.GraphSession, int]:
    s = wb.GraphSession()
    src, gn, snk = s.add_node("source"), s.add_node("gain", {"gain": initial}), s.add_node("sink")
    s.connect(src, 0, gn, 0)
    s.connect(gn, 0, snk, 0)
    return s, gn


def test_tune_recovers_a_gain_and_writes_back():
    s, gn = _gain_session(1.0)                       # wrong start
    rng = np.random.default_rng(0)
    x = rng.standard_normal((1, 512)).astype(np.float32)
    target = x * 0.3                                 # the gain that should be recovered
    stats = wb.tune_to_target(s, x, target, loss_fn=None, steps=300, lr=0.05)

    assert stats["loss_after"] < stats["loss_before"] * 1e-2      # converged
    # the tuned value is written back into the session document (persisted + broadcastable)
    node = next(n for n in s.to_document()["nodes"] if n["id"] == gn)
    assert abs(node["params"]["0"] - 0.3) < 1e-2


def test_tune_uses_mse_when_asked():
    import aiudio.diff as adiff

    s, gn = _gain_session(1.0)
    x = np.random.default_rng(1).standard_normal((1, 256)).astype(np.float32)
    stats = wb.tune_to_target(s, x, x * 0.5, loss_fn=adiff.mse, steps=200, lr=0.05)
    assert stats["loss_after"] < stats["loss_before"]
    assert abs(next(n for n in s.to_document()["nodes"] if n["id"] == gn)["params"]["0"] - 0.5) < 1e-2


def test_tune_is_iterative_idempotent():
    # tuning again starts from the already-tuned params (document overrides → init_params)
    s, _gn = _gain_session(1.0)
    x = np.random.default_rng(2).standard_normal((1, 256)).astype(np.float32)
    wb.tune_to_target(s, x, x * 0.4, steps=200, lr=0.05)
    stats = wb.tune_to_target(s, x, x * 0.4, steps=50, lr=0.02)   # already close → tiny loss
    assert stats["loss_before"] < 0.1

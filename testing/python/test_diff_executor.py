"""D0 — the differentiable executor spine (Phase 1, ADR-0016/0017).

Verifies: the registry has the D0 nodes; the DiffExecutor reads the same `Graph` IR and its torch
forward matches the C++ `process()` within tolerance (**parity**); gradients flow end-to-end and
match the analytic value; the forward is differentiable w.r.t. its input (gradcheck); each node
declares a differentiability status; unknown node types are rejected.

Gated on PyTorch (the optional `aiudio[diff]` layer) — skipped where torch isn't installed, like
the live-device tests.
"""
from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")            # skip the whole module without torch
adiff = pytest.importorskip("aiudio.diff")

import aiudio as a  # noqa: E402

SR = 48000.0
BLOCK = 256


def _gain_graph(gain: float):
    """source → gain → sink. Returns (graph, compiled_executor, gain_node_id)."""
    g = a.Graph()
    src = g.add_source()
    gn = g.add_gain(gain)
    snk = g.add_sink()
    g.connect(src, 0, gn, 0)
    g.connect(gn, 0, snk, 0)
    assert g.validate()[0]
    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    return g, ex, gn


def test_registry_has_d0_nodes():
    types = set(adiff.registered_types())
    assert {"SourceNode", "SinkNode", "GainNode", "SumNode"} <= types


def test_parity_gain():
    g, ex, gn = _gain_graph(0.5)
    de = adiff.DiffExecutor(g, init_params={gn: {0: 0.5}})   # same gain on both sides
    x = np.full((1, BLOCK), 0.8, np.float32)
    d = adiff.assert_parity(ex, de, x, tol=1e-5)
    assert d <= 1e-5
    # sanity on the value itself
    out = de(torch.from_numpy(x).unsqueeze(0)).squeeze(0)
    assert np.allclose(out.detach().numpy(), 0.4, atol=1e-6)   # 0.8 * 0.5


def test_parity_fanout_sum():
    # source → gain_a(0.5), gain_b(0.3) → sum → sink   (one input stream, fan-out)
    g = a.Graph()
    src = g.add_source()
    ga, gb = g.add_gain(0.5), g.add_gain(0.3)
    mix, snk = g.add_sum(2), g.add_sink()
    g.connect(src, 0, ga, 0)
    g.connect(src, 0, gb, 0)
    g.connect(ga, 0, mix, 0)
    g.connect(gb, 0, mix, 1)
    g.connect(mix, 0, snk, 0)
    assert g.validate()[0]
    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    de = adiff.DiffExecutor(g, init_params={ga: {0: 0.5}, gb: {0: 0.3}})
    x = np.full((1, BLOCK), 1.0, np.float32)
    d = adiff.assert_parity(ex, de, x, tol=1e-5)
    assert d <= 1e-5
    out = de(torch.from_numpy(x).unsqueeze(0)).squeeze(0)
    assert np.allclose(out.detach().numpy(), 0.8, atol=1e-6)   # 0.5 + 0.3


def test_gradient_flows_and_matches_analytic():
    g, _ex, gn = _gain_graph(0.5)
    de = adiff.DiffExecutor(g, init_params={gn: {0: 0.5}}, dtype=torch.float64)
    rng = np.random.default_rng(0)
    x = torch.from_numpy(rng.standard_normal((1, 1, BLOCK))).to(torch.float64)
    out = de(x)
    loss = out.pow(2).mean()
    loss.backward()
    gain = dict(de.named_parameters())[f"_diff.{gn}.gain"]
    assert gain.grad is not None and torch.isfinite(gain.grad).all()
    # d/dg mean((g*x)^2) = 2*g*mean(x^2)
    analytic = 2.0 * 0.5 * float((x**2).mean())
    assert abs(float(gain.grad) - analytic) < 1e-9


def test_gradcheck_wrt_input():
    g, _ex, gn = _gain_graph(0.7)
    de = adiff.DiffExecutor(g, init_params={gn: {0: 0.7}}, dtype=torch.float64)
    x = torch.randn(1, 1, 16, dtype=torch.float64, requires_grad=True)
    assert torch.autograd.gradcheck(de, (x,), eps=1e-6, atol=1e-6)


def test_differentiability_status():
    g, _ex, _gn = _gain_graph(0.5)
    de = adiff.DiffExecutor(g)
    report = de.differentiability_report()
    assert set(report.values()) <= {"full", "surrogate", "nondiff"}
    assert all(v == "full" for v in report.values())  # D0 nodes are all fully differentiable


def test_unknown_node_type_is_rejected():
    with pytest.raises(NotImplementedError):
        adiff.make_diff_node("FrobnicateNode", 1, 1, {})

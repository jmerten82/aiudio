"""D3 — nonlinear + recursive diff nodes (Phase 1, ADR-0016).

Waveshaper (tanh/softclip = FULL; hardclip = straight-through SURROGATE) and the recursive
DcBlocker (per-frame scan). Both auto-mirror the graph via the introspection enabler (shape /
corner_hz / sample_rate read from the IR — no init_params). Verifies parity + gradients.
(Stateful Compressor/Gate + feedback Delay need careful differentiable-envelope/BPTT work → a
D3 follow-up; see docs/79.)

Gated on PyTorch.
"""
from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")
adiff = pytest.importorskip("aiudio.diff")

import aiudio as a  # noqa: E402

SR = 48000.0
BLOCK = 256


def _chain(build, channels=1):
    g = a.Graph()
    src, snk = g.add_source(), g.add_sink()
    nid = build(g)
    g.connect(src, 0, nid, 0)
    g.connect(nid, 0, snk, 0)
    ex = a.GraphExecutor()
    assert ex.compile(g, channels=channels, sample_rate=SR, max_block=BLOCK)
    return g, ex, nid


def test_registry_has_d3_nodes():
    assert {"WaveshaperNode", "DcBlockerNode"} <= set(adiff.registered_types())


# ---------------------------------------------------------------- Waveshaper

@pytest.mark.parametrize("shape", ["tanh", "softclip", "hardclip"])
def test_waveshaper_parity(shape):
    g, ex, _ws = _chain(lambda g: g.add_waveshaper(shape, 2.0, 0.7))
    de = adiff.DiffExecutor(g)                                   # auto-reads shape/drive/mix
    rng = np.random.default_rng(3)
    x = (0.8 * rng.standard_normal((1, BLOCK))).astype(np.float32)
    assert adiff.assert_parity(ex, de, x, tol=1e-5) <= 1e-5


def test_waveshaper_differentiability_status():
    g, _ex, _ = _chain(lambda g: g.add_waveshaper("tanh", 1.0, 1.0))
    de = adiff.DiffExecutor(g)
    assert "full" in de.differentiability_report().values()
    g, _ex, _ = _chain(lambda g: g.add_waveshaper("hardclip", 1.0, 1.0))
    de = adiff.DiffExecutor(g)
    assert "surrogate" in de.differentiability_report().values()  # hardclip = straight-through


def test_waveshaper_gradients_flow():
    g, _ex, ws = _chain(lambda g: g.add_waveshaper("tanh", 2.0, 0.5))
    de = adiff.DiffExecutor(g, dtype=torch.float64)
    x = torch.randn(1, 1, BLOCK, dtype=torch.float64)
    de(x).pow(2).mean().backward()
    params = dict(de.named_parameters())
    for name in (f"_diff.{ws}.drive", f"_diff.{ws}.mix"):
        assert params[name].grad is not None and torch.isfinite(params[name].grad).all()


def test_hardclip_straight_through_gradient():
    # a fully-clipped input has zero true gradient; STE lets it flow (≈ identity in the region)
    g, _ex, _ = _chain(lambda g: g.add_waveshaper("hardclip", 1.0, 1.0))
    de = adiff.DiffExecutor(g, dtype=torch.float64)
    x = torch.full((1, 1, 8), 3.0, dtype=torch.float64, requires_grad=True)   # deep in the clip
    de(x).sum().backward()
    assert torch.all(x.grad == 1.0)                              # straight-through → gradient 1


# ---------------------------------------------------------------- DcBlocker (recursive)

def test_dcblocker_parity():
    g, ex, _dc = _chain(lambda g: g.add_dc_blocker(20.0))
    de = adiff.DiffExecutor(g)                                   # reads corner_hz + sample_rate
    rng = np.random.default_rng(4)
    x = (0.5 * rng.standard_normal((1, BLOCK)) + 0.3).astype(np.float32)  # signal + DC offset
    # DcBlocker is STATEFUL (state persists across blocks in C++); the diff forward starts from
    # zero state, so compare the first block (both cold) — warmup=0.
    assert adiff.assert_parity(ex, de, x, tol=1e-4, warmup=0) <= 1e-4


def test_dcblocker_removes_dc():
    g, _ex, _dc = _chain(lambda g: g.add_dc_blocker(20.0))
    de = adiff.DiffExecutor(g)
    x = torch.full((1, 1, 4096), 0.5)                            # pure DC
    out = de(x).squeeze().detach().numpy()
    assert abs(out[-1]) < abs(0.5) * 0.05                        # settled output ≪ input DC


def test_dcblocker_gradient_flows_through_recursion():
    g, _ex, _dc = _chain(lambda g: g.add_dc_blocker(20.0))
    de = adiff.DiffExecutor(g, dtype=torch.float64, sample_rate=SR)
    x = torch.randn(1, 1, 64, dtype=torch.float64, requires_grad=True)
    de(x).pow(2).sum().backward()
    assert x.grad is not None and torch.isfinite(x.grad).all() and torch.any(x.grad != 0)

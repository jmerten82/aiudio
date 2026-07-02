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


# ---------------------------------------------------------------- D3 finish: dynamics + delay

def test_registry_has_dynamics_nodes():
    assert {"CompressorNode", "GateNode", "DelayNode"} <= set(adiff.registered_types())


def test_dynamics_differentiability_is_surrogate():
    for build in (lambda g: g.add_compressor(-24.0, 4.0, 5.0, 80.0),
                  lambda g: g.add_gate(-30.0, 1.0, 120.0, -60.0),
                  lambda g: g.add_delay(0.1, 64, 0.5, 0.5)):
        g, _ex, _n = _chain(build)
        assert "surrogate" in adiff.DiffExecutor(g).differentiability_report().values()


def _loud_transient(seed=7):
    rng = np.random.default_rng(seed)
    x = (0.5 * rng.standard_normal((1, BLOCK))).astype(np.float32)
    x[0, 50:120] = 0.95                                    # a loud burst to trigger dynamics
    return x


def test_compressor_parity_and_reduces_gain():
    g, ex, _c = _chain(lambda g: g.add_compressor(-24.0, 4.0, 5.0, 80.0))
    de = adiff.DiffExecutor(g)                              # stateful → compare cold
    x = _loud_transient()
    assert adiff.assert_parity(ex, de, x, tol=1e-4, warmup=0) <= 1e-4
    out = de(torch.from_numpy(x).unsqueeze(0)).squeeze(0).detach().numpy()
    # the loud burst is attenuated (gain reduction) → its peak drops below the input's
    assert np.max(np.abs(out[0, 60:120])) < np.max(np.abs(x[0, 60:120]))


def test_compressor_gradients_flow():
    g, _ex, c = _chain(lambda g: g.add_compressor(-24.0, 4.0, 5.0, 80.0))
    de = adiff.DiffExecutor(g, dtype=torch.float64)
    x = torch.from_numpy(_loud_transient()).unsqueeze(0).to(torch.float64)
    de(x).pow(2).mean().backward()
    p = dict(de.named_parameters())
    for name in ("threshold_db", "ratio", "attack_ms", "release_ms", "makeup_db"):
        assert p[f"_diff.{c}.{name}"].grad is not None
        assert torch.isfinite(p[f"_diff.{c}.{name}"].grad).all()
    assert p[f"_diff.{c}.threshold_db"].grad != 0            # active (signal exceeds threshold)


def test_gate_parity_and_closes_below_threshold():
    g, ex, _gt = _chain(lambda g: g.add_gate(-20.0, 1.0, 60.0, -60.0))
    de = adiff.DiffExecutor(g)
    x = (0.02 * np.random.default_rng(8).standard_normal((1, BLOCK))).astype(np.float32)  # quiet
    assert adiff.assert_parity(ex, de, x, tol=1e-4, warmup=0) <= 1e-4
    out = de(torch.from_numpy(x).unsqueeze(0)).squeeze(0).detach().numpy()
    assert np.max(np.abs(out[0, -64:])) < np.max(np.abs(x[0, -64:]))   # gated down when quiet


def test_gate_gradients_flow():
    # A signal crossing the threshold both ways (quiet bed + loud burst) exercises opening
    # (attack), closing (release) and the floor (range). NOTE: the gate's threshold_db appears only
    # in the hard `where` *condition*, so it gets no gradient — matching the C++ hard knee (a soft
    # knee would trade away exact parity); a documented limitation, not trained here.
    x = np.full((1, BLOCK), 0.01, np.float32)
    x[0, 60:140] = 0.9
    g, _ex, gt = _chain(lambda g: g.add_gate(-20.0, 1.0, 60.0, -60.0))
    de = adiff.DiffExecutor(g, dtype=torch.float64)
    de(torch.from_numpy(x).unsqueeze(0).to(torch.float64)).pow(2).sum().backward()
    p = dict(de.named_parameters())
    for name in ("attack_ms", "release_ms", "range_db"):     # the differentiable gate params
        grad = p[f"_diff.{gt}.{name}"].grad
        assert grad is not None and torch.isfinite(grad).all()


def test_delay_parity_and_produces_echo():
    g, ex, _d = _chain(lambda g: g.add_delay(0.1, 64, 0.5, 0.5))     # 64-frame delay, observable
    de = adiff.DiffExecutor(g)
    x = np.zeros((1, BLOCK), np.float32)
    x[0, 0] = 1.0                                          # an impulse → echoes at 64, 128, ...
    assert adiff.assert_parity(ex, de, x, tol=1e-4, warmup=0) <= 1e-4
    out = de(torch.from_numpy(x).unsqueeze(0)).squeeze(0).detach().numpy()
    assert abs(out[0, 64]) > 0.1 and abs(out[0, 128]) > 0.01          # feedback echoes present


def test_delay_gradients_flow():
    g, _ex, d = _chain(lambda g: g.add_delay(0.1, 64, 0.5, 0.5))
    de = adiff.DiffExecutor(g, dtype=torch.float64)
    x = torch.from_numpy(_loud_transient(10)).unsqueeze(0).to(torch.float64)
    de(x).pow(2).sum().backward()
    p = dict(de.named_parameters())
    for name in ("feedback", "mix"):
        assert p[f"_diff.{d}.{name}"].grad is not None
        assert torch.isfinite(p[f"_diff.{d}.{name}"].grad).all() and p[f"_diff.{d}.{name}"].grad != 0

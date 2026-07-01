"""D1 — differentiable stateless linear nodes (Phase 1, ADR-0016).

Mixer (N→1, per-input learnable gains, width-preserving) and Pan (1→2 equal-power, learnable pan
— a channel-width change, G8). Each: C++↔torch parity + gradient checks. (DcBlocker/Delay are
recursive → D3; ChannelMatrix needs channel-layout introspection → deferred; see docs/79.)

Gated on PyTorch, like the D0 suite.
"""
from __future__ import annotations

import math

import numpy as np
import pytest

torch = pytest.importorskip("torch")
adiff = pytest.importorskip("aiudio.diff")

import aiudio as a  # noqa: E402

SR = 48000.0
BLOCK = 256


def test_registry_has_d1_nodes():
    assert {"MixerNode", "PanNode"} <= set(adiff.registered_types())


# ---------------------------------------------------------------- Mixer

def _mixer_fanout_graph():
    """source → mixer(2 inputs, both fed by the source) → sink  (single input stream, fan-out).
    Per-input gains default to 1.0 (add_mixer has no per-input ctor); set them via set_param."""
    g = a.Graph()
    src = g.add_source()
    mx = g.add_mixer(2, 1.0)
    snk = g.add_sink()
    g.connect(src, 0, mx, 0)
    g.connect(src, 0, mx, 1)
    g.connect(mx, 0, snk, 0)
    assert g.validate()[0]
    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    return g, ex, mx


def test_mixer_parity_and_value():
    g, ex, mx = _mixer_fanout_graph()
    ex.set_param(mx, 0, 0.5)                            # set the C++ per-input gains (settle via
    ex.set_param(mx, 1, 0.3)                            # the parity harness's warmup blocks)
    de = adiff.DiffExecutor(g, init_params={mx: {0: 0.5, 1: 0.3}})
    x = np.full((1, BLOCK), 1.0, np.float32)
    # set_param drives the mixer's *smoothed* gains: after warmup they settle to within ~1.7e-5
    # of target (a float32 steady-state residual of SmoothedValue). Construction-set params
    # (e.g. add_gain/add_pan) are bit-exact; smoothed params settle within smoother precision.
    assert adiff.assert_parity(ex, de, x, tol=1e-4, warmup=32) <= 1e-4
    out = de(torch.from_numpy(x).unsqueeze(0)).squeeze(0)
    assert np.allclose(out.detach().numpy(), 0.8, atol=1e-6)   # 1*(0.5+0.3)


def test_mixer_gradients_match_analytic():
    g, _ex, mx = _mixer_fanout_graph()
    de = adiff.DiffExecutor(g, init_params={mx: {0: 0.5, 1: 0.3}}, dtype=torch.float64)
    rng = np.random.default_rng(1)
    x = torch.from_numpy(rng.standard_normal((1, 1, BLOCK))).to(torch.float64)
    out = de(x)                                        # = x*(g0+g1)
    out.sum().backward()
    gains = dict(de.named_parameters())[f"_diff.{mx}.gains"]
    # d/dg_i sum(x*(g0+g1)) = sum(x) for each input
    assert torch.allclose(gains.grad, torch.full((2,), float(x.sum()), dtype=torch.float64), atol=1e-9)


# ---------------------------------------------------------------- Pan

def _pan_graph(pan: float):
    """source → pan(1→2) → sink, compiled at 2ch (the pan widens mono → stereo)."""
    g = a.Graph()
    src, pn, snk = g.add_source(), g.add_pan(pan), g.add_sink()
    g.connect(src, 0, pn, 0)
    g.connect(pn, 0, snk, 0)
    assert g.validate()[0]
    ex = a.GraphExecutor()
    assert ex.compile(g, channels=2, sample_rate=SR, max_block=BLOCK)
    return g, ex, pn


@pytest.mark.parametrize("pan", [-0.6, 0.0, 0.5])
def test_pan_parity(pan):
    g, ex, pn = _pan_graph(pan)
    de = adiff.DiffExecutor(g, init_params={pn: {0: pan}})
    # feed a 2-channel input (numpy bridge output width follows input); pan reads channel 0
    x = np.zeros((2, BLOCK), np.float32)
    x[0] = 0.7
    assert adiff.assert_parity(ex, de, x, tol=1e-5) <= 1e-5


def test_pan_value_center():
    g, _ex, pn = _pan_graph(0.0)
    de = adiff.DiffExecutor(g, init_params={pn: {0: 0.0}})
    x = np.zeros((1, 2, BLOCK), np.float32)
    x[0, 0] = 1.0
    out = de(torch.from_numpy(x)).squeeze(0).detach().numpy()
    # centre pan → equal power: cos(π/4)=sin(π/4)=√½
    assert out.shape[0] == 2
    assert np.allclose(out[0], math.sqrt(0.5), atol=1e-6)
    assert np.allclose(out[1], math.sqrt(0.5), atol=1e-6)


def test_pan_gradient_flows():
    g, _ex, pn = _pan_graph(0.2)
    de = adiff.DiffExecutor(g, init_params={pn: {0: 0.2}}, dtype=torch.float64)
    x = torch.zeros(1, 2, BLOCK, dtype=torch.float64)
    x[0, 0] = 0.5
    de(x).pow(2).sum().backward()
    pan_p = dict(de.named_parameters())[f"_diff.{pn}.pan"]
    assert pan_p.grad is not None and torch.isfinite(pan_p.grad).all() and pan_p.grad != 0


def test_pan_gradcheck_wrt_input():
    g, _ex, pn = _pan_graph(-0.3)
    de = adiff.DiffExecutor(g, init_params={pn: {0: -0.3}}, dtype=torch.float64)
    x = torch.randn(1, 2, 16, dtype=torch.float64, requires_grad=True)
    assert torch.autograd.gradcheck(de, (x,), eps=1e-6, atol=1e-6)

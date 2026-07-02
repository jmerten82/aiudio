"""D5 — parameter matching against a target render (Phase 1).

The headline slice: given a target rendered by a graph with *known* parameters, recover them from a
wrong init by gradient descent through the (audio-domain, multi-node) differentiable graph. Uses
D0–D4 end to end (DiffExecutor + nodes + loss + `match_target`). Gated on PyTorch.
"""
from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")
adiff = pytest.importorskip("aiudio.diff")

import aiudio as a  # noqa: E402

SR = 48000.0
BLOCK = 512


def test_recover_waveshaper_params():
    # source → waveshaper(tanh) → sink; recover drive + mix from a saturating target render.
    g = a.Graph()
    s, ws, k = g.add_source(), g.add_waveshaper("tanh", 1.0, 1.0), g.add_sink()
    g.connect(s, 0, ws, 0)
    g.connect(ws, 0, k, 0)
    a.GraphExecutor().compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    rng = np.random.default_rng(0)
    x = torch.from_numpy((2.0 * rng.standard_normal((4, 1, BLOCK))).astype(np.float32))  # drives tanh
    target = adiff.DiffExecutor(g, init_params={ws: {0: 3.0, 1: 0.6}})(x).detach()       # true params

    learner = adiff.DiffExecutor(g, init_params={ws: {0: 1.0, 1: 1.0}})                  # wrong init
    hist = adiff.match_target(learner, x, target, steps=600, lr=0.05, seed=0)
    assert hist[-1] < hist[0] * 1e-2                                                     # converged
    p = dict(learner.named_parameters())
    assert abs(p[f"_diff.{ws}.drive"].item() - 3.0) < 0.3                                # recovered
    assert abs(p[f"_diff.{ws}.mix"].item() - 0.6) < 0.05
    # and the render matches
    assert adiff.max_abs_diff(learner(x), target) < 1e-2


def test_match_target_render_multinode():
    # source → gain → waveshaper → compressor → sink: recover a whole channel-strip's params so its
    # RENDER matches the target. Params can entangle (gain vs. compressor threshold), so the
    # acceptance is the render match + a big loss drop (the "brighten/shape to match" goal).
    g = a.Graph()
    s = g.add_source()
    gn = g.add_gain(1.0)
    ws = g.add_waveshaper("tanh", 1.0, 1.0)
    cp = g.add_compressor(-18.0, 4.0, 5.0, 80.0)
    k = g.add_sink()
    for x_, y_ in [(s, gn), (gn, ws), (ws, cp), (cp, k)]:
        g.connect(x_, 0, y_, 0)
    a.GraphExecutor().compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    rng = np.random.default_rng(1)
    x = torch.from_numpy((0.6 * rng.standard_normal((2, 1, 256))).astype(np.float32))
    true = {gn: {0: 0.7}, ws: {0: 2.5, 1: 0.4}, cp: {0: -24.0, 1: 6.0}}
    target = adiff.DiffExecutor(g, init_params=true)(x).detach()

    learner = adiff.DiffExecutor(g)                                                      # default init
    hist = adiff.match_target(learner, x, target, steps=500, lr=0.03, seed=0)
    assert hist[-1] < hist[0] * 1e-2                                                     # big loss drop
    assert adiff.max_abs_diff(learner(x), target) < 2e-2                                 # render matches


def test_match_target_with_stft_loss():
    # spectral objective on a tone through gain → waveshaper
    g = a.Graph()
    s, gn, ws, k = g.add_source(), g.add_gain(1.0), g.add_waveshaper("tanh", 1.0, 1.0), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, ws, 0)
    g.connect(ws, 0, k, 0)
    a.GraphExecutor().compile(g, channels=1, sample_rate=SR, max_block=4096)

    t = torch.arange(4096, dtype=torch.float32)
    x = (1.5 * torch.sin(2 * torch.pi * 220.0 * t / SR)).reshape(1, 1, 4096)
    target = adiff.DiffExecutor(g, init_params={gn: {0: 0.8}, ws: {0: 3.0, 1: 0.7}})(x).detach()

    learner = adiff.DiffExecutor(g)
    hist = adiff.match_target(learner, x, target, loss_fn=adiff.MultiResolutionSTFTLoss(),
                              steps=350, lr=0.03, seed=0)
    assert hist[-1] < hist[0] * 0.1                                                      # spectral match improves
    assert adiff.max_abs_diff(learner(x), target) < 5e-2


def test_recovered_params_export_to_cpp_preview():
    # D5→D6 hint: after matching, the learned params are plain floats — write them into the C++
    # graph via set_param (the round-trip that D6 formalizes).
    g = a.Graph()
    s, gn, k = g.add_source(), g.add_gain(1.0), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    x = torch.from_numpy(np.random.default_rng(3).standard_normal((2, 1, BLOCK)).astype(np.float32))
    target = x * 0.35
    learner = adiff.DiffExecutor(g)
    adiff.match_target(learner, x, target, steps=300, lr=0.05, seed=0)
    learned = dict(learner.named_parameters())[f"_diff.{gn}.gain"].item()
    assert ex.set_param(gn, 0, learned)                                                 # push to C++
    assert abs(learned - 0.35) < 1e-2

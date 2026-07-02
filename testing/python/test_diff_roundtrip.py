"""D6 — round-trip to real time (Phase 1).

Close the loop: write a trained DiffExecutor's params into the compiled C++ graph (`export_to_graph`
→ `set_param`), then assert the **C++ render matches the trained-torch render** (the parity harness,
now going torch → C++). Covers the two regimes: atomic-param + stateful (gain → compressor, compared
cold), and smoothed params (gain → waveshaper, compared after the smoother settles). Gated on PyTorch.
"""
from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")
adiff = pytest.importorskip("aiudio.diff")

import aiudio as a  # noqa: E402

SR = 48000.0
BLOCK = 512


def test_export_writes_param_count():
    g = a.Graph()
    s = g.add_source()
    gn = g.add_gain(1.0)
    cp = g.add_compressor(-18.0, 4.0, 5.0, 80.0)
    k = g.add_sink()
    for x_, y_ in [(s, gn), (gn, cp), (cp, k)]:
        g.connect(x_, 0, y_, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    diff = adiff.DiffExecutor(g)
    assert adiff.export_to_graph(diff, ex) == 1 + 5     # gain (1) + compressor (5)


def test_roundtrip_atomic_stateful():
    # gain → compressor: atomic params + stateful envelope. Trained params (≠ C++ construction)
    # exported to C++ reproduce the trained render, compared cold (warmup=0 — both start fresh).
    g = a.Graph()
    s = g.add_source()
    gn = g.add_gain(1.0)                                 # C++ construction defaults…
    cp = g.add_compressor(-18.0, 4.0, 5.0, 80.0)
    k = g.add_sink()
    for x_, y_ in [(s, gn), (gn, cp), (cp, k)]:
        g.connect(x_, 0, y_, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    # a "trained" diff model with different params
    trained = adiff.DiffExecutor(g, init_params={gn: {0: 0.6},
                                                 cp: {0: -24.0, 1: 6.0, 2: 3.0, 3: 100.0, 4: 2.0}})
    adiff.export_to_graph(trained, ex)                   # write them into the C++ graph
    rng = np.random.default_rng(0)
    x = (0.6 * rng.standard_normal((1, BLOCK))).astype(np.float32)
    x[0, 50:120] = 0.95
    assert adiff.assert_parity(ex, trained, x, warmup=0, tol=1e-4) <= 1e-4   # C++ == trained render


def test_roundtrip_smoothed():
    # gain → waveshaper: drive/mix are SmoothedValue, so warm up to let them settle after export.
    g = a.Graph()
    s, gn, ws, k = g.add_source(), g.add_gain(1.0), g.add_waveshaper("tanh", 1.0, 1.0), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, ws, 0)
    g.connect(ws, 0, k, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    trained = adiff.DiffExecutor(g, init_params={gn: {0: 0.5}, ws: {0: 2.0, 1: 0.7}})
    adiff.export_to_graph(trained, ex)
    x = (1.2 * np.random.default_rng(1).standard_normal((1, BLOCK))).astype(np.float32)
    assert adiff.assert_parity(ex, trained, x, warmup=32, tol=1e-4) <= 1e-4  # settles → matches


def test_full_roundtrip_train_then_deploy():
    # end-to-end: match a target in torch, export to C++, the C++ render reproduces the target.
    g = a.Graph()
    s, gn, k = g.add_source(), g.add_gain(1.0), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    rng = np.random.default_rng(2)
    x_np = (rng.standard_normal((1, BLOCK))).astype(np.float32)
    x = torch.from_numpy(x_np).unsqueeze(0)
    target = x * 0.35
    learner = adiff.DiffExecutor(g)
    adiff.match_target(learner, x, target, steps=300, lr=0.05, seed=0)   # train gain → 0.35
    adiff.export_to_graph(learner, ex)                                   # deploy to C++
    cpp_out = ex.process(x_np)                                           # C++ render
    assert np.allclose(cpp_out, x_np * 0.35, atol=2e-3)                  # reproduces the target

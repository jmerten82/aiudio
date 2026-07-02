"""D4 — losses + training harness (Phase 1).

Multi-resolution STFT loss (properties + differentiable) and the `fit` harness (converges,
deterministic under a seed, checkpoints round-trip). Gated on PyTorch.
"""
from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")
adiff = pytest.importorskip("aiudio.diff")

import aiudio as a  # noqa: E402

SR = 48000.0
BLOCK = 512


def _tone(freq, n=4096, amp=0.5, sr=SR):
    t = torch.arange(n, dtype=torch.float32)
    return (amp * torch.sin(2 * torch.pi * freq * t / sr)).reshape(1, 1, n)


# ---------------------------------------------------------------- multi-res STFT loss

def test_stft_loss_zero_for_identical():
    loss = adiff.MultiResolutionSTFTLoss()
    x = _tone(440.0)
    assert float(loss(x, x)) < 1e-5


def test_stft_loss_positive_for_different():
    loss = adiff.MultiResolutionSTFTLoss()
    assert float(loss(_tone(440.0), _tone(880.0))) > 0.1


def test_stft_loss_differentiable():
    loss = adiff.MultiResolutionSTFTLoss()
    x = _tone(440.0).clone().requires_grad_(True)
    loss(x, _tone(500.0)).backward()
    assert x.grad is not None and torch.isfinite(x.grad).all() and torch.any(x.grad != 0)


def test_stft_loss_handles_short_blocks():
    # fft sizes larger than the signal are clamped — no crash on a short block
    loss = adiff.MultiResolutionSTFTLoss(fft_sizes=(512, 1024, 2048))
    x = _tone(440.0, n=256)
    assert float(loss(x, x)) < 1e-4


# ---------------------------------------------------------------- fit harness

def _gain_model(init_gain: float):
    g = a.Graph()
    s, gn, k = g.add_source(), g.add_gain(init_gain), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    a.GraphExecutor().compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    de = adiff.DiffExecutor(g)
    gid = [nid for (nid, t, _i, _o) in g.nodes() if t == "GainNode"][0]
    return de, gid


def test_fit_recovers_gain_with_mse():
    de, gid = _gain_model(1.0)                      # start wrong
    rng = np.random.default_rng(0)
    x = torch.from_numpy(rng.standard_normal((4, 1, BLOCK)).astype(np.float32))
    target = x * 0.3
    hist = adiff.fit(de, adiff.mse, lambda: (x, target), steps=300, lr=0.05, seed=0)
    assert hist[-1] < hist[0] * 1e-3               # converged
    gain = dict(de.named_parameters())[f"_diff.{gid}.gain"]
    assert abs(gain.item() - 0.3) < 1e-2           # recovered the gain


def test_fit_recovers_gain_with_stft_loss():
    de, gid = _gain_model(1.0)
    x = _tone(300.0)
    target = x * 0.4
    hist = adiff.fit(de, adiff.MultiResolutionSTFTLoss(), lambda: (x, target),
                     steps=400, lr=0.05, seed=0)
    assert hist[-1] < hist[0]
    gain = dict(de.named_parameters())[f"_diff.{gid}.gain"]
    assert abs(gain.item() - 0.4) < 5e-2           # STFT loss recovers the scaling


def test_fit_is_deterministic_under_seed():
    x = torch.from_numpy(np.random.default_rng(1).standard_normal((2, 1, BLOCK)).astype(np.float32))
    target = x * 0.5
    de1, _ = _gain_model(1.0)
    h1 = adiff.fit(de1, adiff.mse, lambda: (x, target), steps=50, lr=0.05, seed=42)
    de2, _ = _gain_model(1.0)
    h2 = adiff.fit(de2, adiff.mse, lambda: (x, target), steps=50, lr=0.05, seed=42)
    assert h1 == h2                                # identical loss history


def test_checkpoint_roundtrips(tmp_path):
    de, gid = _gain_model(1.0)
    x = torch.from_numpy(np.random.default_rng(2).standard_normal((2, 1, BLOCK)).astype(np.float32))
    adiff.fit(de, adiff.mse, lambda: (x, x * 0.25), steps=200, lr=0.05, seed=0)
    trained = dict(de.named_parameters())[f"_diff.{gid}.gain"].item()

    path = str(tmp_path / "ckpt.pt")
    adiff.save_checkpoint(de, path)
    de2, gid2 = _gain_model(1.0)                    # a fresh model (gain 1.0)
    adiff.load_checkpoint(de2, path)
    restored = dict(de2.named_parameters())[f"_diff.{gid2}.gain"].item()
    assert abs(restored - trained) < 1e-9          # params restored exactly

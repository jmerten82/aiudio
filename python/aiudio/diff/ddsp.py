"""A DDSP-style differentiable synth — the Phase 1 · D8 exemplar.

`HarmonicSynth` is a differentiable **harmonic + filtered-noise** generator at a *fixed* pitch (the
DDSP additive model, ICLR 2020). Its per-harmonic amplitudes and a noise gain are learnable, so it
trains — with the multi-resolution STFT loss (D4) — to **match the timbre / spectral envelope** of a
target sound.

Why fixed pitch: a multi-res STFT loss is poor at pitch (docs/20 §2.1) — f0 is not learned by naive
descent. Fixing f0 and learning the spectral envelope is exactly what the STFT loss is good at, and
is the honest, tractable D8 slice. Pitch-aware / staged training is a Phase 2+ concern.

    synth = HarmonicSynth(f0=220.0, n_harmonics=32)
    adiff.fit(synth, adiff.MultiResolutionSTFTLoss(), lambda: (None, target), steps=800)
"""
from __future__ import annotations

import math

import torch
import torch.nn as nn
import torch.nn.functional as F


class HarmonicSynth(nn.Module):
    """Additive harmonic bank at a fixed ``f0`` + white-noise residual, both differentiable.

    Learnable: ``amps`` (one per harmonic, passed through softplus → non-negative) and
    ``noise_gain`` (log-domain). Harmonics above Nyquist are masked out. The noise buffer is fixed
    at construction so ``forward`` is deterministic (seed before building for reproducibility).
    ``forward(x=None)`` ignores its argument — it's a generator — so it drops straight into `fit`.
    """

    def __init__(self, f0: float = 220.0, n_harmonics: int = 32, sample_rate: float = 48000.0,
                 n_samples: int = 8192, noise: bool = True):
        super().__init__()
        self.f0 = float(f0)
        self.sample_rate = float(sample_rate)
        self.n_samples = int(n_samples)
        self.n_harmonics = int(n_harmonics)
        self.amps = nn.Parameter(torch.zeros(n_harmonics))        # softplus(amps) = harmonic gains
        self.noise_gain = nn.Parameter(torch.tensor(-8.0))        # log gain (≈0 at init)

        k = torch.arange(1, n_harmonics + 1, dtype=torch.float32)  # harmonic numbers 1..H
        t = torch.arange(self.n_samples, dtype=torch.float32) / self.sample_rate
        basis = torch.sin(2.0 * math.pi * (k[:, None] * self.f0) * t[None, :])   # [H, N]
        below_nyquist = (k * self.f0 < 0.5 * self.sample_rate).to(torch.float32)[:, None]
        self.register_buffer("basis", basis * below_nyquist)      # zero out aliasing harmonics
        self.register_buffer(
            "noise_buf", torch.randn(self.n_samples) if noise else torch.zeros(self.n_samples))

    @property
    def harmonic_amplitudes(self) -> torch.Tensor:
        """The (non-negative) per-harmonic amplitudes — the learned spectral envelope."""
        return F.softplus(self.amps.detach())

    def forward(self, x: torch.Tensor | None = None) -> torch.Tensor:
        dt = self.basis.dtype
        amps = F.softplus(self.amps).to(dt)
        harmonic = (amps[:, None] * self.basis).sum(dim=0)        # [N]
        out = harmonic + torch.exp(self.noise_gain).to(dt) * self.noise_buf
        return out.reshape(1, 1, self.n_samples)

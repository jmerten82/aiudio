"""Training losses for the differentiable core (Phase 1 · D4).

The workhorse for audio is the **multi-resolution STFT loss** (Yamamoto et al. / DDSP): compare
magnitude spectrograms at several FFT resolutions, combining spectral convergence with a
log-magnitude L1. It captures spectral/timbral structure far better than raw-sample MSE.

Implemented directly on ``torch.stft`` (no extra dependency — the formula is standard and small;
`auraloss` is a drop-in alternative if you prefer). Plain ``mse``/``l1`` are also exposed for
direct signal matching. All are differentiable (autograd) and shape-agnostic over
``[batch, channels, frames]`` or ``[batch, frames]``.

Caveat (`docs/theory/20` §2.1): a multi-res STFT loss is **poor at pitch** — it won't train an
oscillator's frequency by naive descent (D8 needs pitch-aware losses / staged training).
"""
from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F


def mse(pred: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    """Mean-squared error over the waveform."""
    return F.mse_loss(pred, target)


def l1(pred: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    """Mean absolute error over the waveform."""
    return F.l1_loss(pred, target)


class MultiResolutionSTFTLoss(nn.Module):
    """Sum of single-resolution STFT losses over several FFT sizes.

    Each resolution contributes **spectral convergence** ``‖|Y|−|X|‖_F / ‖|Y|‖_F`` plus a
    **log-magnitude L1** ``mean|log|X| − log|Y||``. FFT sizes larger than the signal are clamped so
    it's robust to short blocks. Differentiable end-to-end.
    """

    def __init__(self, fft_sizes: tuple[int, ...] = (512, 1024, 2048), hop_ratio: float = 0.25,
                 eps: float = 1e-7):
        super().__init__()
        self.fft_sizes = tuple(fft_sizes)
        self.hop_ratio = float(hop_ratio)
        self.eps = float(eps)

    def _stft_mag(self, x: torch.Tensor, n_fft: int) -> torch.Tensor:
        hop = max(1, int(n_fft * self.hop_ratio))
        window = torch.hann_window(n_fft, device=x.device, dtype=x.dtype)
        spec = torch.stft(x, n_fft=n_fft, hop_length=hop, win_length=n_fft, window=window,
                          center=True, return_complex=True)
        return spec.abs()

    def _one_resolution(self, x: torch.Tensor, y: torch.Tensor, n_fft: int) -> torch.Tensor:
        mag_x = self._stft_mag(x, n_fft)
        mag_y = self._stft_mag(y, n_fft)
        sc = torch.linalg.norm(mag_y - mag_x) / (torch.linalg.norm(mag_y) + self.eps)
        logmag = F.l1_loss(torch.log(mag_x + self.eps), torch.log(mag_y + self.eps))
        return sc + logmag

    def forward(self, pred: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
        # collapse (batch, channels, frames) → (batch*channels, frames) for torch.stft
        p = pred.reshape(-1, pred.shape[-1])
        t = target.reshape(-1, target.shape[-1])
        n = p.shape[-1]
        losses = [self._one_resolution(p, t, min(fft, n)) for fft in self.fft_sizes]
        return torch.stack(losses).mean()

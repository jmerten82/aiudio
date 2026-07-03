"""Trainable filters (Phase 1 · D2, candidate ADR-0018).

Direct-form IIR coefficients have poor gradients, and the recursive structure resists autodiff
(``docs/theory/20`` §2.2). The well-conditioned workaround: parameterize a biquad by its **design
parameters** (cutoff, Q, gain_dB) — *reparameterized* for optimization (log-frequency,
softplus-Q) — and train through the **analytic magnitude response** ``|H(e^jω)|`` rather than the
time-domain recursion. The design→coefficient math is **identical to the C++
``BiquadNode::design()``** (RBJ cookbook), so trained parameters export to a C++ `BiquadNode`
faithfully — the bridge to real time (D6).

`DiffBiquad` is a standalone trainable filter (not a graph node). Running a biquad *inside* a
differentiable graph forward needs the recursive time-domain scan — that lands with the recursive
nodes in D3.
"""
from __future__ import annotations

import enum
import math

import torch
import torch.nn as nn
import torch.nn.functional as F


class FilterType(enum.Enum):
    LOWPASS = "lowpass"
    HIGHPASS = "highpass"
    PEAKING = "peaking"
    LOWSHELF = "lowshelf"
    HIGHSHELF = "highshelf"


class DiffBiquad(nn.Module):
    """A differentiable RBJ biquad in design-parameter space (learnable cutoff/Q/gain_dB)."""

    def __init__(self, filter_type, freq: float, q: float, gain_db: float = 0.0,
                 sample_rate: float = 48000.0, dtype: torch.dtype = torch.float64):
        super().__init__()
        self.filter_type = filter_type if isinstance(filter_type, FilterType) else FilterType(filter_type)
        self.sample_rate = float(sample_rate)
        # reparameterized so optimization is well-conditioned (docs/pipeline/79 §3):
        self.log_freq = nn.Parameter(torch.tensor(math.log(float(freq)), dtype=dtype))
        self.raw_q = nn.Parameter(torch.tensor(math.log(math.expm1(max(float(q), 1e-3))), dtype=dtype))
        self.gain_db = nn.Parameter(torch.tensor(float(gain_db), dtype=dtype))

    @property
    def freq(self) -> torch.Tensor:
        return torch.exp(self.log_freq)

    @property
    def q(self) -> torch.Tensor:
        return F.softplus(self.raw_q) + 1e-3

    def coefficients(self):
        """(b0,b1,b2,a1,a2), a0-normalized — exactly matching C++ BiquadNode::design()."""
        w0 = 2.0 * math.pi * self.freq / self.sample_rate
        cw, sw = torch.cos(w0), torch.sin(w0)
        alpha = sw / (2.0 * torch.clamp(self.q, min=1e-6))
        t = self.filter_type
        if t is FilterType.LOWPASS:
            b0 = (1 - cw) * 0.5
            b1 = 1 - cw
            b2 = (1 - cw) * 0.5
            a0 = 1 + alpha
            a1 = -2 * cw
            a2 = 1 - alpha
        elif t is FilterType.HIGHPASS:
            b0 = (1 + cw) * 0.5
            b1 = -(1 + cw)
            b2 = (1 + cw) * 0.5
            a0 = 1 + alpha
            a1 = -2 * cw
            a2 = 1 - alpha
        elif t is FilterType.PEAKING:
            A = torch.exp(self.gain_db * (math.log(10.0) / 40.0))
            b0 = 1 + alpha * A
            b1 = -2 * cw
            b2 = 1 - alpha * A
            a0 = 1 + alpha / A
            a1 = -2 * cw
            a2 = 1 - alpha / A
        elif t is FilterType.LOWSHELF:
            A = torch.exp(self.gain_db * (math.log(10.0) / 40.0))
            sq = 2.0 * torch.sqrt(A) * alpha
            b0 = A * ((A + 1) - (A - 1) * cw + sq)
            b1 = 2 * A * ((A - 1) - (A + 1) * cw)
            b2 = A * ((A + 1) - (A - 1) * cw - sq)
            a0 = (A + 1) + (A - 1) * cw + sq
            a1 = -2 * ((A - 1) + (A + 1) * cw)
            a2 = (A + 1) + (A - 1) * cw - sq
        else:  # HIGHSHELF
            A = torch.exp(self.gain_db * (math.log(10.0) / 40.0))
            sq = 2.0 * torch.sqrt(A) * alpha
            b0 = A * ((A + 1) + (A - 1) * cw + sq)
            b1 = -2 * A * ((A - 1) + (A + 1) * cw)
            b2 = A * ((A + 1) + (A - 1) * cw - sq)
            a0 = (A + 1) - (A - 1) * cw + sq
            a1 = 2 * ((A - 1) - (A + 1) * cw)
            a2 = (A + 1) - (A - 1) * cw - sq
        return b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0

    def magnitude_response(self, freqs_hz) -> torch.Tensor:
        """|H(e^jω)| at ``freqs_hz`` (differentiable)."""
        b0, b1, b2, a1, a2 = self.coefficients()
        cdtype = torch.complex128 if b0.dtype == torch.float64 else torch.complex64
        w = 2.0 * math.pi * torch.as_tensor(freqs_hz, dtype=b0.dtype) / self.sample_rate
        z1 = torch.exp(-1j * w.to(cdtype))
        z2 = z1 * z1
        num = b0.to(cdtype) + b1.to(cdtype) * z1 + b2.to(cdtype) * z2
        den = 1.0 + a1.to(cdtype) * z1 + a2.to(cdtype) * z2
        return torch.abs(num / den)

    def export_coeffs(self) -> tuple[float, float, float, float, float]:
        """(b0,b1,b2,a1,a2) as floats — feed to ``Graph.add_biquad_coeffs`` for real time (D6)."""
        return tuple(float(c.detach()) for c in self.coefficients())


def fit_magnitude(model: DiffBiquad, target_mag, freqs_hz, *, steps: int = 500,
                  lr: float = 0.05) -> list[float]:
    """Fit ``model``'s design params to a target magnitude response (log-magnitude MSE). Returns
    the per-step loss history. The reparameterization keeps this well-conditioned (docs/theory/20 §2.2)."""
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    target = torch.as_tensor(target_mag, dtype=model.log_freq.dtype)
    history: list[float] = []
    for _ in range(steps):
        opt.zero_grad()
        pred = model.magnitude_response(freqs_hz)
        loss = ((torch.log(pred + 1e-9) - torch.log(target + 1e-9)) ** 2).mean()
        loss.backward()
        opt.step()
        history.append(float(loss.detach()))
    return history

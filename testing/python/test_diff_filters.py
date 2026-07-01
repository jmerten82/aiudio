"""D2 — trainable filters (Phase 1, candidate ADR-0018).

`DiffBiquad` trains a filter by its design params (cutoff/Q/gain_dB, reparameterized) through the
analytic magnitude response, then exports to the C++ `BiquadNode` for real time. Verifies:
- **formula parity** — DiffBiquad's analytic |H| matches the C++ RBJ design (via the impulse
  response of `add_biquad_*`), so the coefficient math is identical;
- **fit** — gradient descent recovers a target magnitude response;
- **export round-trip** — trained coeffs → `add_biquad_coeffs` → C++ response matches (feeds D6);
- gradients flow through the design params.

Gated on PyTorch.
"""
from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")
adiff = pytest.importorskip("aiudio.diff")

import aiudio as a  # noqa: E402

SR = 48000.0
NFFT = 8192


def _cpp_biquad_response(node_builder, freqs_hz):
    """Impulse-response magnitude |H(f)| of a C++ biquad graph at freqs_hz."""
    g = a.Graph()
    src = g.add_source()
    bq = node_builder(g)
    snk = g.add_sink()
    g.connect(src, 0, bq, 0)
    g.connect(bq, 0, snk, 0)
    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=NFFT)
    imp = np.zeros((1, NFFT), np.float32)
    imp[0, 0] = 1.0
    h = ex.process(imp)[0]                       # impulse response
    mag = np.abs(np.fft.rfft(h))
    # sample the FFT magnitude at the requested frequencies (nearest bin)
    idx = np.clip(np.round(np.asarray(freqs_hz) / (SR / NFFT)).astype(int), 0, len(mag) - 1)
    return mag[idx]


def _log_freqs(n=64, lo=30.0, hi=18000.0):
    return np.geomspace(lo, hi, n)


def test_formula_parity_lowpass():
    freqs = _log_freqs()
    cpp = _cpp_biquad_response(lambda g: g.add_biquad_lowpass(4000.0, 0.707, SR), freqs)
    dbq = adiff.DiffBiquad("lowpass", 4000.0, 0.707, sample_rate=SR)
    torch_mag = dbq.magnitude_response(freqs).detach().numpy()
    # impulse-response FFT vs analytic |H|: agree closely away from FFT-truncation edges
    assert np.max(np.abs(torch_mag - cpp)) < 2e-2


def test_formula_parity_peaking():
    freqs = _log_freqs()
    cpp = _cpp_biquad_response(lambda g: g.add_biquad_peaking(2000.0, 2.0, 6.0, SR), freqs)
    dbq = adiff.DiffBiquad("peaking", 2000.0, 2.0, 6.0, sample_rate=SR)
    torch_mag = dbq.magnitude_response(freqs).detach().numpy()
    assert np.max(np.abs(torch_mag - cpp)) < 3e-2


def test_fit_recovers_target_response():
    freqs = _log_freqs(96)
    target = adiff.DiffBiquad("peaking", 2000.0, 2.0, 6.0, sample_rate=SR)
    target_mag = target.magnitude_response(freqs).detach()

    learner = adiff.DiffBiquad("peaking", 600.0, 1.0, 0.0, sample_rate=SR)  # wrong init
    hist = adiff.fit_magnitude(learner, target_mag, freqs, steps=800, lr=0.05)
    assert hist[-1] < hist[0] * 1e-2                    # loss dropped ≥ 100x
    final = learner.magnitude_response(freqs).detach()
    assert torch.max(torch.abs(final - target_mag)) < 5e-2
    # recovered the design params reasonably well
    assert abs(learner.freq.item() - 2000.0) / 2000.0 < 0.1
    assert abs(learner.gain_db.item() - 6.0) < 0.6


def test_export_roundtrip_to_cpp():
    freqs = _log_freqs()
    dbq = adiff.DiffBiquad("peaking", 3000.0, 1.5, -4.0, sample_rate=SR)
    b0, b1, b2, a1, a2 = dbq.export_coeffs()
    cpp = _cpp_biquad_response(lambda g: g.add_biquad_coeffs(b0, b1, b2, a1, a2), freqs)
    torch_mag = dbq.magnitude_response(freqs).detach().numpy()
    assert np.max(np.abs(torch_mag - cpp)) < 3e-2       # exported coeffs realize the trained response


def test_gradients_flow_through_design_params():
    freqs = _log_freqs(32)
    dbq = adiff.DiffBiquad("peaking", 1000.0, 1.0, 0.0, sample_rate=SR)
    loss = dbq.magnitude_response(freqs).pow(2).mean()
    loss.backward()
    for name in ("log_freq", "raw_q", "gain_db"):
        p = dict(dbq.named_parameters())[name]
        assert p.grad is not None and torch.isfinite(p.grad).all()

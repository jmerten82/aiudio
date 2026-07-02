"""D8 — DDSP synth exemplar (Phase 1, final milestone).

`HarmonicSynth` (harmonic + filtered-noise, fixed f0) trains to match a target **timbre** via the
multi-resolution STFT loss — recovering the spectral envelope. Fixed pitch by design (STFT loss is
poor at pitch, docs/20 §2.1). Gated on PyTorch.
"""
from __future__ import annotations

import pytest

torch = pytest.importorskip("torch")
adiff = pytest.importorskip("aiudio.diff")

SR = 48000.0
N = 8192
F0 = 220.0


def _target_synth(profile):
    """A HarmonicSynth whose softplus(amps) equals `profile` (a known spectral envelope)."""
    adiff.seed_everything(0)
    synth = adiff.HarmonicSynth(F0, n_harmonics=len(profile), sample_rate=SR, n_samples=N, noise=False)
    with torch.no_grad():
        synth.amps.copy_(torch.log(torch.expm1(torch.tensor(profile))))  # inverse softplus
    return synth


def test_synth_generates_harmonic_audio():
    synth = adiff.HarmonicSynth(F0, n_harmonics=16, sample_rate=SR, n_samples=N, noise=False)
    with torch.no_grad():
        # a decaying envelope so the fundamental dominates
        synth.amps.copy_(torch.log(torch.expm1(torch.tensor([1.0 / (k + 1) for k in range(16)]))))
        out = synth()
    assert out.shape == (1, 1, N)
    # spectrum peaks at the fundamental f0
    mag = torch.fft.rfft(out.reshape(-1)).abs()
    freqs = torch.fft.rfftfreq(N, 1.0 / SR)
    assert abs(freqs[mag.argmax()].item() - F0) < SR / N            # peak within one bin of f0


def test_synth_harmonics_above_nyquist_are_masked():
    synth = adiff.HarmonicSynth(F0, n_harmonics=400, sample_rate=SR, n_samples=N, noise=False)
    k = torch.arange(1, 401)
    above = (k * F0 >= 0.5 * SR)
    assert torch.all(synth.basis[above] == 0)                       # aliasing harmonics zeroed


def test_synth_is_differentiable():
    synth = adiff.HarmonicSynth(F0, n_harmonics=16, sample_rate=SR, n_samples=1024)
    synth().pow(2).mean().backward()
    assert synth.amps.grad is not None and torch.isfinite(synth.amps.grad).all()
    assert synth.noise_gain.grad is not None and torch.isfinite(synth.noise_gain.grad).all()


def test_synth_matches_target_timbre():
    profile = [1.0 / (k + 1) for k in range(32)]                    # a 1/n harmonic rolloff
    target = _target_synth(profile)
    target_audio = target().detach()

    adiff.seed_everything(0)
    learner = adiff.HarmonicSynth(F0, n_harmonics=32, sample_rate=SR, n_samples=N, noise=False)
    stft = adiff.MultiResolutionSTFTLoss()
    with torch.no_grad():
        before = stft(learner(), target_audio).item()
    hist = adiff.fit(learner, stft, lambda: (None, target_audio), steps=400, lr=0.05)
    with torch.no_grad():
        after = stft(learner(), target_audio).item()

    assert after < before * 0.05                                    # spectral distance collapses
    assert hist[-1] < hist[0]
    # the learned spectral envelope recovers the target profile
    amp_err = (learner.harmonic_amplitudes - target.harmonic_amplitudes).abs().max().item()
    assert amp_err < 0.02

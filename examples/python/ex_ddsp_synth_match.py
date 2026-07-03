#!/usr/bin/env python3
"""Example: a DDSP-style differentiable synth matches a target timbre (Phase 1 · D8).

The Phase-1 capstone. A differentiable harmonic + filtered-noise synth (`HarmonicSynth`, the DDSP
additive model) learns the **spectral envelope** of a target sound by gradient descent on the
multi-resolution STFT loss — the ML-first workflow end to end (build → loss → train → metrics).

Pitch is fixed (a multi-res STFT loss is poor at pitch — docs/theory/20 §2.1); the synth learns *timbre*
at a known f0, which is exactly what the loss is good at. A perceptual metric (CLAP embedding
distance) is the Phase-2 hook noted below.

    pip install "aiudio[diff]"
    python examples/python/ex_ddsp_synth_match.py
"""
from __future__ import annotations

try:
    import aiudio.diff as adiff
    import torch
except ModuleNotFoundError as exc:
    raise SystemExit(f'this example needs the diff layer: pip install "aiudio[diff]"  ({exc})')

SR, N, F0 = 48000.0, 8192, 220.0


def main() -> None:
    adiff.seed_everything(0)

    # A target timbre: a "brassy" profile (strong low harmonics, gentle rolloff) at f0.
    profile = torch.tensor([1.0 / (k + 1) ** 0.8 for k in range(48)])
    target = adiff.HarmonicSynth(F0, n_harmonics=48, sample_rate=SR, n_samples=N, noise=False)
    with torch.no_grad():
        target.amps.copy_(torch.log(torch.expm1(profile)))          # softplus(amps) == profile
    target_audio = target().detach()

    # Train a fresh synth (flat/zero envelope) to match it — multi-resolution STFT loss (D4).
    learner = adiff.HarmonicSynth(F0, n_harmonics=48, sample_rate=SR, n_samples=N, noise=False)
    stft = adiff.MultiResolutionSTFTLoss()
    with torch.no_grad():
        before = stft(learner(), target_audio).item()
    hist = adiff.fit(learner, stft, lambda: (None, target_audio), steps=600, lr=0.05)
    with torch.no_grad():
        after = stft(learner(), target_audio).item()

    amp_err = (learner.harmonic_amplitudes - target.harmonic_amplitudes).abs().max().item()
    print(f"multi-res STFT distance: {before:.3f} -> {after:.4f}  (loss {hist[0]:.3f} -> {hist[-1]:.4f})")
    print(f"spectral-envelope match: max harmonic-amplitude error {amp_err:.4f}")
    print("first 6 harmonics — learned vs target:")
    for k in range(6):
        print(f"  h{k + 1}: {learner.harmonic_amplitudes[k]:.3f}  ({target.harmonic_amplitudes[k]:.3f})")
    print("\nmetric = multi-res STFT distance. Perceptual CLAP-embedding distance is the Phase-2 hook.")


if __name__ == "__main__":
    main()

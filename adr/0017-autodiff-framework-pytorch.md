# ADR-0017: Autodiff framework — PyTorch (optional `aiudio[diff]` extra)

- **Status:** Accepted
- **Date:** 2026-07-01
- **Deciders:** Project owner + Claude Code
- **Related:** ADR-0016 (differentiable execution), ADR-0002 (C++ core + Python ML layer),
  ADR-0006 (runtime-agnostic neural inference), [`docs/pipeline/79`](../docs/pipeline/79-phase1-differentiable-core-roadmap.md),
  [`docs/theory/20`](../docs/theory/20-differentiable-dsp-and-neural-audio.md)

## Context

ADR-0016 introduces a Python differentiable executor; it needs an **autodiff substrate**. This is
a major dependency choice (an ADR trigger per `adr/README.md`). Constraints/forces:
- The differentiable layer is **Python** (ADR-0002: Python = research/ML). The substrate should be
  a mainstream Python autodiff/tensor library.
- **Neural-audio gravity** (✓ `docs/theory/20`): DDSP, RAVE, EnCodec/DAC, `auraloss` (multi-resolution
  STFT loss), NAM, and most neural-audio research are **PyTorch**-first. Interop with that
  ecosystem is a first-order concern for pillars 1 and 3.
- **Deployment path to RT** (ADR-0006): the RT inference story (RTNeural/ONNX/LibTorch, ANIRA) has
  clean export routes from PyTorch (TorchScript, `torch.onnx`).
- **Weight/cost:** it is a large dependency (hundreds of MB). The base `aiudio` package (the RT
  core + control frontend) must stay lightweight — most users of the RT pipeline don't train.

## Decision

**We will use PyTorch as the autodiff/tensor substrate for aiudio's differentiable & ML layer, and
ship it as an *optional* extra `aiudio[diff]` (not a base dependency).** The `aiudio.diff`
subpackage imports torch **lazily** and raises a clear, actionable error
(`pip install "aiudio[diff]"`) if it's absent, so the base install (RT core + bindings + numpy)
is unaffected.

## Consequences

**Positive**
- Direct access to the neural-audio ecosystem: `auraloss`/`torchaudio` losses, DDSP/RAVE-class
  models, pretrained checkpoints, GPU/MPS acceleration.
- Clean export routes toward RT deployment (TorchScript/ONNX → ADR-0006).
- float32 tensors match the engine's canonical sample type (`docs/theory/73` §4).
- Base package stays lightweight; torch is pulled only by those who train.

**Negative / costs**
- A heavy dependency for the `diff` extra (download/disk); version churn to track.
- Ties the differentiable layer to one framework's API (mitigated: it's isolated in
  `aiudio.diff`, behind the registry/executor, not spread through the codebase).

**Neutral / follow-ups**
- CI runs the `diff` tests where torch is available; they are **skipped** (gated) where it isn't —
  same pattern as the live-device tests. On macOS the wheel is CPU/MPS (no CUDA fork).
- Loss libraries (`auraloss`) and datasets/checkpointing land with milestone D4.

## Alternatives considered

- **JAX** — excellent autodiff/XLA, but neural-*audio* tooling, pretrained models, and the RT
  export ecosystem are markedly thinner than PyTorch's; worse fit for pillars 1/3.
- **A custom / minimal autodiff** — rejected: reinventing a mature substrate (against CLAUDE.md §7
  "don't reinvent"); no ecosystem.
- **torch as a base dependency** — rejected: bloats the base package for the majority who only run
  the RT pipeline; the optional extra keeps concerns separated.
- **TensorFlow** — weaker fit with the neural-audio research corpus and the C++/export story.

## References
- [`docs/theory/20`](../docs/theory/20-differentiable-dsp-and-neural-audio.md) (DDSP/RAVE/codecs/auraloss — PyTorch-centric),
  [`docs/pipeline/79`](../docs/pipeline/79-phase1-differentiable-core-roadmap.md) §3/§7.
- ADR-0016 (differentiable execution), ADR-0002 (Python ML layer), ADR-0006 (RT neural deployment).

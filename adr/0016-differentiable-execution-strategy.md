# ADR-0016: Differentiable execution — a Python/PyTorch executor over the same IR

- **Status:** Accepted
- **Date:** 2026-07-01
- **Deciders:** Project owner + Claude Code
- **Related:** [`docs/pipeline/79`](../docs/pipeline/79-phase1-differentiable-core-roadmap.md) (Phase 1 roadmap, D0),
  [`docs/theory/50`](../docs/theory/50-architecture-patterns.md) §3, [`docs/theory/20`](../docs/theory/20-differentiable-dsp-and-neural-audio.md) §2,
  ADR-0002, ADR-0003, ADR-0004, ADR-0009, ADR-0006, ADR-0017

## Context

Phase 1 delivers pillar 3 of the vision (`docs/00`): the graph is **differentiable**, so its
parameters can be optimized against an audio objective (✓ precedent: differentiable
mixing-graph reverse-engineering, arXiv:2406.01049; Text2FX param tuning — `docs/theory/50` §3). Invariant
§4.3 already commits to "one IR, many backends: real-time, offline, **and differentiable**
executors run the same IR," and the node contract reserves "differentiable parameters" +
"differentiability status" (§4.4/§4.7). Phase 1 must *realize* that third executor.

Forces:
- **The RT path must not change** (ADR-0004): training is heavy, batched, allocation-happy,
  GPU-friendly — the opposite of the audio thread. It belongs in the Python research/ML layer
  (ADR-0002), never on the audio thread.
- **One IR, not two** (ADR-0003/0009): the differentiable executor must derive from the *same*
  `Graph`, not a hand-maintained parallel description that can silently diverge.
- **Some DSP is hard to differentiate** (✓ `docs/theory/20` §2): recursive IIR filters resist autodiff;
  oscillator-frequency gradients are uninformative; hard clips have no gradient. A node must be
  able to **declare** how differentiable it is.
- **The point is to reach real time**: trained parameters have to end up back in the C++ RT graph.

## Decision

**We will add a third executor — a Python/PyTorch `DiffExecutor` — that interprets the same C++
`Graph` IR and evaluates each node through a differentiable `forward()`, with autograd end-to-end.**
Specifically:

1. **The `DiffExecutor` reads the existing `Graph`** via the bindings' introspection
   (`nodes()`, `edges()`, `node_type()`), builds the computation in the graph's topological order,
   and evaluates over a batched tensor **`[batch, channels, frames]`**. It lives in an **optional**
   Python subpackage `aiudio.diff` (torch imported lazily) — the base package needs no torch.
2. **Each node type has a dual face:** its C++ `process()` (RT/offline) *and* a Python torch
   `forward(inputs, params) -> tensor` (training), connected by a **registry** (`type_name → diff
   node`). Node parameters become `torch.nn.Parameter`s (constrained/reparameterized where
   optimization demands it — freq in log-space, ratios via softplus, etc.).
3. **Each diff node declares a differentiability status** — `Full` / `Surrogate` (trainable via an
   approximation, e.g. SVF for filters, straight-through for hard clip) / `NonDiff` (frozen,
   pass-through with no grad) — enforcing invariant §4.7.
4. **A parity harness gates every node:** the torch `forward()` must match the C++ `process()`
   within tolerance on the same input+params. This is what keeps the two implementations honest
   and lets trained parameters round-trip to C++ (Phase-1 milestone D6).
5. **The bridge to RT is parameters, not audio:** training happens in torch; results are written
   back into the C++ `Graph` (`set_param` / coefficient setters) and re-verified by parity.
   RT *deployment* of neural models (RTNeural/ONNX on the audio thread) stays ADR-0006 / Phase 3.

## Consequences

**Positive**
- Realizes the ML-first pillar and invariant §4.3 without touching the audio thread (ADR-0004
  intact — Phase 1 adds *zero* RT code).
- One IR stays authoritative: the differentiable executor is a *second interpreter* of the same
  `Graph`, mirroring how the offline backend is a second *driver* of the RT executor.
- Gradient-based parameter optimization ("match/brighten") and, later, agent-tuned graphs
  (Phase 2) become possible.

**Negative / costs**
- **Dual implementation per node** (C++ `process()` + torch `forward()`) — duplicated math that
  can drift. *Mitigation:* the mandatory parity harness in CI; share/derive coefficient math where
  possible; document intended divergences (surrogates).
- Hard/ill-conditioned ops need surrogates/STE and honest `NonDiff` labels (`docs/theory/20` §2).
- A heavy optional dependency (PyTorch) enters the project (ADR-0017).

**Neutral / follow-ups**
- Enforces the node contract's differentiability fields (CLAUDE.md §4.7) — now a tested property.
- Trainable-filter representation (SVF/frequency-sampling) is its own decision at D2 (candidate
  ADR-0018).
- Multi-stream source/sink binding in the diff executor (stream indices) is a follow-up once
  needed (D1+); D0 covers the single-input-stream case.

## Alternatives considered

- **A hand-written Python graph mirror** (user rebuilds the topology in torch) — rejected: two
  sources of truth that drift; violates ADR-0003 "one IR."
- **Differentiate the C++ engine directly** (e.g. Enzyme/AD over C++) — rejected: enormous
  complexity, poor fit for the research/ML workflow, and it would entangle the RT core with
  training concerns (against ADR-0002/0004).
- **JAX instead of PyTorch** — see ADR-0017.
- **Defer differentiability entirely** — rejected: it is pillar 3 of the locked vision.

## References
- [`docs/pipeline/79`](../docs/pipeline/79-phase1-differentiable-core-roadmap.md) (D0–D8, foundation abstractions, hard problems),
  [`docs/theory/50`](../docs/theory/50-architecture-patterns.md) §3, [`docs/theory/20`](../docs/theory/20-differentiable-dsp-and-neural-audio.md) §2.
- ADR-0003/0009 (one IR + node contract), ADR-0004 (audio thread sacred), ADR-0002 (C++ core + Python ML layer),
  ADR-0006 (runtime-agnostic neural inference — deployment), ADR-0017 (PyTorch).

# ADR-0006: Runtime-agnostic neural inference (inline RTNeural + off-thread ANIRA)

- **Status:** Accepted
- **Date:** 2026-06-28
- **Deciders:** Project owner
- **Related:** `docs/theory/30-*` §2–§3, ADR-0004, ADR-0003

## Context

Neural nodes vary enormously in cost: a small amp-model runs per-block in
microseconds; a RAVE-class synth or separator cannot meet a 2.7 ms inline budget.
Verified (`docs/theory/30-*`, ✓): **RTNeural** runs small models RT-safe *inline* by
pre-allocating all memory; **ANIRA** runs heavy models by **decoupling inference
to a static thread pool** off the audio callback, and proves **one RT-safe layer
can wrap multiple ML runtimes (LibTorch / ONNX Runtime / TFLite)** selectable at
runtime.

## Decision

**We will support two execution classes for neural nodes behind the node
contract, and abstract over the underlying ML runtime — never binding the engine
to a single one:**

1. **Inline RT-safe nodes** — small/causal models on the audio thread (RTNeural
   pattern).
2. **Off-thread pooled nodes** — heavy models dispatched to a pre-allocated thread
   pool, results returned a block later with reported latency (ANIRA pattern).

The runtime (LibTorch/ONNX/TFLite/RTNeural) is an implementation detail of a node,
chosen per node.

## Consequences

**Positive**
- Both small and large neural nodes fit the RT engine without compromising
  ADR-0004; freedom to pick the best runtime per model.
- Can reuse ANIRA/RTNeural directly rather than rebuilding inference.

**Negative / costs**
- Off-thread nodes add latency (a block or more) that the graph must compensate.
- A runtime-abstraction layer to design and maintain; quantization/format details
  per backend.

**Neutral / follow-ups**
- The scheduler must choose inline vs pooled per node from its `realtime_capable`
  / latency metadata (ADR-0003).

## Alternatives considered

- **Single ML runtime (e.g. LibTorch only)** — simpler, but locks us out of
  ONNX/TFLite footprint/quantization advantages and embedded targets.
- **Everything off-thread** — needless latency for small models that run fine
  inline.

## References

- `docs/theory/30-*`, RTNeural (arXiv:2106.03037), ANIRA (arXiv:2506.12665) — `docs/90-references.md`.

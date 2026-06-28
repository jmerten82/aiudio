# ADR-0002: C++ real-time core + Python research/ML layer

- **Status:** Accepted
- **Date:** 2026-06-28
- **Deciders:** Project owner
- **Related:** `docs/00-vision-and-scope.md` §3.4, `docs/50-architecture-patterns.md` §5

## Context

aiudio must satisfy two conflicting needs: a **real-time-safe** audio engine
(hard latency, no GC pauses, plugin hosting) and a **research-friendly ML
workflow** (PyTorch/JAX, fast iteration, the agent). One language optimizes for
neither end well. The verified landscape (`docs/30-*`) shows the entire
production RT-neural ecosystem (RTNeural, ANIRA, JUCE, LibTorch) is C++, while the
ML/research ecosystem is Python.

## Decision

**We will implement the real-time core in C++20 and the research/ML layer in
Python (3.11+), with a strict boundary between them.** Python authors, trains, and
controls; C++ executes audio. **Python never runs on the audio thread.** The
graph is serialized to a language-neutral IR + Torch/ONNX artifacts; C++ loads and
runs it without calling back into Python on the audio path. Control-rate changes
cross via a lock-free queue.

## Consequences

**Positive**
- Each layer uses its best-fit ecosystem; real-time safety is structurally
  protected from Python's GIL/allocation.
- Direct reuse of C++ RT tooling (RTNeural, ANIRA, JUCE) and Python ML tooling.

**Negative / costs**
- A binding/interop boundary to design and maintain (see ADR backlog: nanobind).
- Two toolchains, two test setups, a more complex build/packaging story.

**Neutral / follow-ups**
- Mandates the real-time-safety invariant (ADR-0004) and the serialize-don't-call
  boundary rule (`CLAUDE.md` §6).

## Alternatives considered

- **Pure Python** — cannot meet hard-real-time; GIL + allocation are disqualifying.
- **Pure C++** — loses the ML/research velocity and the agent ecosystem.
- **Rust core** — strong RT + WASM story, but a smaller plugin/DSP/ML-runtime
  ecosystem; noted as a future alternative, not the path (`docs/10-*` §5).

## References

- `docs/00-vision-and-scope.md`, `docs/50-architecture-patterns.md` §5, `docs/30-*`.

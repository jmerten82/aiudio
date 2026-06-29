# ADR-0004: Real-time safety — the audio thread is sacred

- **Status:** Accepted
- **Date:** 2026-06-28
- **Deciders:** Project owner
- **Related:** `docs/30-*` §1, `docs/50-architecture-patterns.md` §4, ADR-0002, ADR-0006

## Context

A real-time audio callback runs on a high-priority thread with a hard deadline (at
48 kHz / 128 frames ≈ **2.7 ms** per block); any miss is an audible glitch.
Verified (`docs/30-*`, ✓): mainstream PyTorch/TensorFlow are **unsafe on the audio
thread** because they allocate during inference. The whole RT-neural tooling
landscape (RTNeural's pre-allocation; ANIRA's off-thread pool) exists to obey this
constraint.

## Decision

**We will treat the audio thread as sacred. Inside any real-time callback /
`process()` / device IOProc there is: no heap allocation, no locks/mutexes, no
syscalls/IO, no logging, no exceptions, no Python (GIL), no unbounded work.** All
memory is pre-allocated at `prepare()`/`open()`. Anything crossing a thread
boundary uses **lock-free SPSC ring buffers / atomics**. RT functions are
`noexcept`.

## Consequences

**Positive**
- Deterministic, glitch-free execution; a clear, testable correctness bar.
- Forces the inline-vs-off-thread node split (ADR-0006) to be explicit.

**Negative / costs**
- More upfront design (pre-sizing, lock-free structures); some convenient
  patterns are banned in the hot path.
- Requires RT-safety testing (allocation hooks / RTSan, xrun counters).

**Neutral / follow-ups**
- Heavy neural nodes that can't meet the budget inline must run off-thread
  (ADR-0006) and report latency for compensation.

## Alternatives considered

- **"Mostly real-time" / best-effort** — unacceptable for music production;
  intermittent glitches are disqualifying.
- **Run ML frameworks directly on the audio thread** — verified unsafe (allocation
  during inference).

## References

- `docs/30-*` §1, `docs/50-architecture-patterns.md` §4, RTNeural & ANIRA papers
  (`docs/90-references.md`).

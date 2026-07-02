# ADR-0003: Hybrid signal graph — one typed IR, many backends, universal node contract

- **Status:** Accepted
- **Date:** 2026-06-28
- **Deciders:** Project owner
- **Related:** `docs/theory/50-architecture-patterns.md` §1–§3, `docs/theory/20-*` §1, ADR-0004, ADR-0005

## Context

The core thesis (`docs/00-*`, `docs/theory/60-*`) is that classic DSP and neural models
should be **first-class peers** in one graph, usable in real-time *and* offline,
*and* differentiable for training, *and* editable by an agent. These four demands
(RT / offline / differentiable / editable) pull in different directions. DDSP
(`docs/theory/20-*`, ✓) proves DSP blocks and neural nets can be jointly trained when
both expose differentiable parameters; the differentiable mixing-graph result
(arXiv:2406.01049, ✓) proves a whole processor DAG can be optimized by gradients.

## Decision

**We will represent the audio program as a single typed, serializable graph IR,
executed by multiple backends over that same IR** (real-time, offline,
differentiable), and **every node — classic DSP or neural — honors one universal
node contract:**

1. `process(block) → block` (forward render),
2. **differentiable parameters** (+ optional custom `backward`),
3. metadata: `realtime_capable`, latency, causal?, sample-rate, differentiability
   status.

Neither DSP nor neural is privileged; the agent and the optimizer reason over the
whole graph.

## Consequences

**Positive**
- DSP/neural composability and end-to-end trainability fall out of one contract.
- "Train offline, run live" is the *same* IR under different executors.
- The agent's action space is a typed, validatable structure (enables ADR-0008-to-be).

**Negative / costs**
- Forcing four execution semantics through one IR is hard; some nodes need custom
  backward passes (IIR filters) or are non-differentiable and must declare it.
- Risk of an over-general IR; mitigated by building it from real vertical slices.

**Neutral / follow-ups**
- `realtime_capable` and differentiability status become explicit, enforced
  properties (`CLAUDE.md` §4).

## Alternatives considered

- **Separate RT engine and ML engine** with an export bridge (Neutone-style) —
  simpler per-side, but breaks the "peers in one graph" thesis and duplicates
  representations.
- **Neural-only or DSP-only graph** — neither matches the hybrid vision.

## References

- `docs/theory/50-architecture-patterns.md`, `docs/theory/20-*` §1/§3, `docs/theory/60-*` §2.

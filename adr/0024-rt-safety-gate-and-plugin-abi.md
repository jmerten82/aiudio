# ADR-0024: RT-safety gate & node-plugin loading ABI for authored nodes

- **Status:** Accepted
- **Date:** 2026-07-03
- **Deciders:** Project owner + Claude Code
- **Related:** [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) §5.1/§5.1a,
  ADR-0004, ADR-0009, ADR-0010, ADR-0022, ADR-0023

## Context

Locked scope (`docs/pipeline/85` §3): authored nodes may run in **full real-time** — on the audio
thread — from day one. But **the audio thread is sacred** (ADR-0004): no heap allocation, no locks,
no syscalls/logging/exceptions, no unbounded work in `process()`. So we must **enforce** that
invariant on *agent-generated* code, and we must **load** a compiled node into an already-running
engine without disturbing the audio thread. Isolation (ADR-0023) already removes the shipping-repo
risk, so the residual concern is narrow: a bad node glitching/crashing the user's *own local
session* — recoverable, but worth catching before it reaches the live thread.

## Decision

**We will gate every authored node with an automatic RT-safety pre-flight before it may run on the
audio thread, and load nodes through a stable off-thread plugin ABI.**

1. **Automatic pre-flight** (no human ceremony): (a) **static analysis** of `process()` rejecting
   forbidden calls (alloc/locks/syscalls/logging/exceptions/unbounded loops); (b) the existing
   **sanitizers** (ASan/UBSan/TSan) + a **real-time-safety check** (RTSan or an allocation hook on
   `process()`); (c) **golden-render + node-contract tests**.
2. **Fail → auto-discard or quarantine to the non-RT executors** (offline/differentiable) — throwing
   away broken code needs no approval. **Pass → eligible for the audio thread.**
3. **Loading ABI:** compiled node plugins are loaded via a **stable node-plugin ABI** —
   `dlopen` + factory registration happen **off the audio thread**, and insertion into the live
   graph uses the existing **RCU recompile + atomic swap** (ADR-0010).
4. **Two other gates sit around this one** (don't conflate): the **RT-invasive-load consent**
   (active notify + confirm) is ADR-0022; the **heavyweight review** is at promote-to-`main`
   (ADR-0023). This ADR is the *automatic correctness pre-flight*. **ADR-0004 is enforced, never
   relaxed.**

## Consequences

**Positive**
- Full-RT self-extension with the sacred-thread invariant *enforced on generated code* (not
  trusted); fail-safe by discard/quarantine; off-thread load keeps the audio thread undisturbed.

**Negative / costs**
- Static + runtime RT checks and a versioned plugin ABI to build; local-compilation code-execution
  risk (mitigated: sandboxed build, no-network policy for generated nodes, local-only, single-user).

**Neutral / follow-ups**
- Milestones D1 (scaffold + build), D2 (this gate), D3 (agent authors end-to-end). The pre-flight is
  the technical enforcement of ADR-0004 for authored code — a new enforced property alongside
  CLAUDE.md §4.

## Alternatives considered

- **Non-RT only for authored nodes** — rejected by the locked scope (full RT from day one).
- **Trust the agent (no gate)** — violates ADR-0004; a single bad `process()` breaks the audio thread.
- **Require human review for every local RT-load** — too heavy for a personal tool; that review is
  reserved for promote-to-`main` (ADR-0023). Local RT-load is gated by the automatic pre-flight +
  the invasive-change consent (ADR-0022).

## References
- [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) §5.1 (the gate), §5.1a (consent), §6 (D1–D3).
- ADR-0004 (sacred audio thread), ADR-0009 (node contract), ADR-0010 (RCU recompile + atomic swap),
  ADR-0022 (invasive-change consent), ADR-0023 (personal registry + promote-to-PR review).

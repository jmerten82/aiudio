# ADR-0022: Agent runtime & human-in-the-loop policy (RT-invasive consent)

- **Status:** Accepted
- **Date:** 2026-07-03
- **Deciders:** Project owner + Claude Code
- **Related:** [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) §5.1a,
  [`docs/theory/40`](../docs/theory/40-ai-agents-for-audio.md),
  ADR-0004, ADR-0010, ADR-0016, ADR-0020, ADR-0021, ADR-0024

## Context

Phase 2's agent designs and changes the graph from natural language. It must be **grounded** (act
only within real capabilities, ADR-0021), act through the **action space** (ADR-0020), tune
parameters well, and — critically — must **never silently make invasive changes to the sacred RT
audio thread** (ADR-0004). The project owner set an explicit requirement: *invasive changes to the
RT audio thread and its components require active user notification **and** explicit confirmation.*
Separately, a multi-resolution objective/gradient descent already recovers good parameter values
(ADR-0016), whereas an LLM guessing numeric params is weaker (`docs/theory/40`).

## Decision

**We will run the agent as Claude with tool-use bound to the action space (ADR-0020), grounded by
the capability manifest (ADR-0021), governed by a three-gate consent model:**

1. **Routine edits auto-apply** — `set_param`/automation within a node's declared range are
   click-free and reversible via the action log; no prompt.
2. **RT-invasive changes require active notification + explicit confirmation** — loading an
   authored node onto the audio thread; changes to latency/PDC, sample rate, block size, device, or
   clock; replacing/removing a node or subgraph in a live path carrying audio to hardware. The
   workbench **surfaces the change and its impact**, and the agent may **propose/stage but cannot
   commit** without the user's approval (`docs/pipeline/85` §5.1a). This is the control-plane
   enforcement of ADR-0004.
3. **Structure by the LLM, parameters by gradients** — the agent chooses topology/node choices;
   fine parameter tuning is handed to the differentiable layer (`match_target`, ADR-0016). The
   closed loop is render → measure (CLAP / loudness / spectral) → self-correct.

Every action is **validated against the manifest** before apply; the **action log** gives
undo/replay/audit.

## Consequences

**Positive**
- A grounded, auditable, human-in-the-loop agent; the RT thread is never mutated silently.
- The structure/param split plays to each tool's strength (LLM reasoning + gradient precision).

**Negative / costs**
- A confirm UX and the invasive-vs-routine classification to build; LLM reliability still needs
  guardrails (manifest validation + undo are the backstops).

**Neutral / follow-ups**
- Milestones C0/C1/C2. The consent rule is a Phase-2 definition-of-done criterion.

## Alternatives considered

- **Full autonomy (auto-apply everything)** — unsafe: silent RT-invasive or destructive edits.
  Rejected by the owner's requirement + ADR-0004.
- **No gradient hand-off (LLM guesses params)** — weaker parameter quality; wastes the Phase-1
  differentiable machinery.
- **Confirm *every* edit** — too much friction; routine reversible edits don't warrant it (that's
  why the classification exists).

## References
- [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) §5.1a, §6 (C0–C2),
  [`docs/theory/40`](../docs/theory/40-ai-agents-for-audio.md).
- ADR-0004 (sacred audio thread), ADR-0016 (differentiable param tuning), ADR-0020 (action space),
  ADR-0021 (manifest grounding), ADR-0024 (the pre-flight gate that consent sits atop).

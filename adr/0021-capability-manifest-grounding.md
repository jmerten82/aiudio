# ADR-0021: Capability manifest — the single grounding source of truth

- **Status:** Accepted
- **Date:** 2026-07-03
- **Deciders:** Project owner + Claude Code
- **Related:** [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md),
  [`docs/theory/40`](../docs/theory/40-ai-agents-for-audio.md) (agent reliability),
  ADR-0009, ADR-0020, ADR-0022

## Context

Both the UI (node palette, typed parameter editors, connection validation) and the agent (which
nodes exist, their params/ranges, whether they are RT-capable or differentiable) must know the
**real** capabilities of the pipeline. Two failure modes to avoid: a **hardcoded catalog** in the UI
or agent that drifts from the actual node registry, and an **agent that hallucinates** nodes or
parameters that don't exist — the central agent-reliability risk (`docs/theory/40`). The node
contract already exposes `typeName`, `paramValue`, `config`, and metadata (ADR-0009), but not
human-facing param descriptors (names/ranges/defaults/units).

## Decision

**We will generate a machine-readable capability manifest from the live node registry, and make it
the single grounding source consumed by both the UI and the agent.**

1. The manifest (JSON) lists, per node type: its **parameters** (index, name, range, default, unit),
   **ports / channel behavior**, and **metadata** (`realtime_capable`, differentiability status,
   latency, causal). It is produced by **introspecting the registry** — never hand-maintained.
2. It grounds the **UI** (palette + range-checked editors) and the **agent** (its system context /
   tool schemas are built from it), and is the basis for **validating actions** (ADR-0020).
3. Realizing it requires a small **node-introspection extension** in C++: per-parameter descriptors
   (name/range/default/unit) alongside the existing `paramValue`/`config`.

## Consequences

**Positive**
- No drift, no hallucination: the manifest *is* the truth; UI and agent are grounded identically.
- Enables action validation, typed editors, and honest agent capability boundaries.

**Negative / costs**
- A C++ node-contract extension (param descriptors) — a small, additive change to every node
  (touches CLAUDE.md §4.7's "explicit node properties" area).

**Neutral / follow-ups**
- Milestone A1 — the hinge the UI (B), agent (C), and node-authoring (D) all depend on.
- Authored nodes (ADR-0023) must emit manifest descriptors too, so they appear in the palette/agent.

## Alternatives considered

- **Hardcoded catalog** in the UI/agent — drifts from the registry; the exact agent-reliability
  failure we're avoiding. Rejected.
- **Per-tool schemas duplicated** in the agent layer — another drift surface; derive from one
  manifest instead.

## References
- [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) §4 (A1),
  [`docs/theory/40`](../docs/theory/40-ai-agents-for-audio.md).
- ADR-0009 (node contract), ADR-0020 (validation), ADR-0022 (agent grounding).

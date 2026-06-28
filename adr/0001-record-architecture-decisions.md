# ADR-0001: Record architecture decisions (use ADRs)

- **Status:** Accepted
- **Date:** 2026-06-28
- **Deciders:** Project owner
- **Related:** `adr/README.md`, `CLAUDE.md` §9 (living-docs protocol)

## Context

aiudio is an ambitious, multi-layer framework (hybrid DSP+neural graph, agent
control plane, real-time + offline, C++ + Python). Decisions made early —
threading model, the node contract, language split — will constrain everything
built on top. Without a durable record of *why* each choice was made, future
sessions (human or Claude) risk silently re-litigating settled questions or
diverging from the architecture.

## Decision

**We will record architecturally-significant decisions as ADRs** in `/adr`, one
file per decision, using the format in `adr/template.md` (adapted from Michael
Nygard). ADRs are **append-only**: an Accepted ADR is never rewritten; a changed
decision is captured by a **new** ADR that supersedes the old one.

## Consequences

**Positive**
- A consistent, honest trail of how and why the architecture evolved.
- New contributors/sessions orient fast and stop re-deciding settled things.
- Forces explicit articulation of trade-offs and alternatives.

**Negative / costs**
- Small per-decision overhead; discipline required to keep the index current.

**Neutral / follow-ups**
- The ADR index and the README ADR list are part of the `CLAUDE.md` §9
  maintenance protocol — update them in the same change as the decision.

## Alternatives considered

- **Decisions only in `/docs` prose** — design docs evolve and lose the
  point-in-time rationale; no clear "this was decided" signal.
- **Commit messages / issues only** — not discoverable or durable enough for
  load-bearing architecture choices.

## References

- Michael Nygard, *Documenting Architecture Decisions* (2011).
- `adr/README.md` (process), `CLAUDE.md` §9.

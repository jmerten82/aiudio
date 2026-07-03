# ADR-0023: Personal node registry & isolation

- **Status:** Accepted
- **Date:** 2026-07-03
- **Deciders:** Project owner + Claude Code
- **Related:** [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) §3,
  ADR-0009, ADR-0021, ADR-0024

## Context

Phase 2 lets the agent **author new nodes on the fly** when the existing library can't express
something. The owner's constraint: authored nodes must be **reusable** but must **not** land in
shipping `main` automatically — they stay **personal and local**, and a bad one can simply be
thrown away. This isolation is what makes self-extension safe from a *supply-chain* standpoint: the
shipping product is never at risk regardless of what the agent generates.

## Decision

**We will store agent/user-authored nodes in a local node-package registry, isolated from shipping
`main`, reusable across sessions, with an explicit promote-to-PR path.**

1. **Package format:** a node package = manifest descriptors (ADR-0021) + C++ source + tests +
   registration glue.
2. **Local registry:** a **git-ignored** plugin directory (e.g. `~/.aiudio/nodes/` or a workspace
   dir) that the engine **auto-discovers and loads** at startup; versioned; **reusable** in later
   sessions.
3. **Never auto-merged.** The only route into the shipping product is an explicit **promote → PR to
   `main`**, reviewed like any code (the heavyweight review gate; the local RT-safety pre-flight is
   ADR-0024).
4. **Disposable.** Isolation is the primary safety property — a failing or unwanted node is deleted;
   nothing leaks into `main` or to other users.

## Consequences

**Positive**
- Self-extension with **zero supply-chain risk** to the shipping product; genuine reuse; a clean,
  explicit boundary between "my local nodes" and "the product."

**Negative / costs**
- A package format, local registry, discovery + versioning to build; a local build toolchain
  dependency (compiling authored nodes on the user's machine).

**Neutral / follow-ups**
- Milestone D0. Loading + RT-safety of these packages is ADR-0024; their palette/agent visibility
  comes from the manifest (ADR-0021).

## Alternatives considered

- **A personal git branch** of the repo — heavier (rebasing churn, whole-repo scope); offered but
  not chosen (`docs/pipeline/85` §3). The local dir is lighter for fast iteration.
- **Authored nodes committed to the repo** — no isolation; pollutes `main`; defeats the constraint.
- **In-memory only (not persisted)** — not reusable across sessions.

## References
- [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) §3, §6 (D0).
- ADR-0009 (node contract), ADR-0021 (manifest), ADR-0024 (RT-safety gate + plugin loading).

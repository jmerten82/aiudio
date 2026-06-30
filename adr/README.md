# Architecture Decision Records (ADRs)

This directory holds **aiudio's architecture decisions** — the *why* behind the
load-bearing choices, recorded as they are made so we **stay consistent as the
project grows**.

> An ADR captures a single architecturally-significant decision: its context, the
> decision itself, and its consequences. ADRs are **append-only history** — once
> Accepted, an ADR is not rewritten; if the decision changes, a **new** ADR
> supersedes it. This gives us a durable, honest trail of how the architecture
> evolved.

See **[ADR-0001](0001-record-architecture-decisions.md)** for the decision to use
ADRs at all (the format is adapted from Michael Nygard's).

---

## How ADRs relate to the rest of the repo

| Artifact | Role |
|---|---|
| **`/adr`** | **Decisions** — point-in-time, immutable record of *what we chose and why*. |
| **`/docs`** | **Design & research** — living dossier, architecture explanation, plans. Evolves freely. |
| **`README.md`** | Vision, roadmap, status. Links the accepted ADRs. |
| **`CLAUDE.md`** | Working guidance; its invariants are the *enforcement* of accepted ADRs. |

Rule of thumb: **`/docs` explains how the system works today; `/adr` records why it
got that way.** When a doc and an Accepted ADR conflict, the newer-dated source
wins and the older should be updated or superseded.

## When is an ADR required?

Write an ADR for an **architecturally-significant** decision — one that is costly
to reverse or that constrains future work. Examples:

- Changes to a core **invariant** (real-time safety, the node contract, one-IR).
- **Language / runtime / threading** boundaries.
- The **public API or graph IR** shape.
- A **major dependency** or build-system choice (e.g. ML runtime, plugin format).
- **Platform / I/O** strategy.

**Do *not* write an ADR** for easily-reversible, local, or stylistic choices —
those live in code review and `CLAUDE.md` conventions.

## Lifecycle (status)

```
Proposed ──▶ Accepted ──▶ Deprecated
                │
                └────────▶ Superseded by ADR-XXXX
```

- **Proposed** — under discussion; not yet binding.
- **Accepted** — binding; the codebase must conform.
- **Deprecated** — no longer relevant, not replaced.
- **Superseded by ADR-XXXX** — replaced by a newer decision (link it; never
  silently edit the old one).

## Workflow

1. Copy [`template.md`](template.md) to `NNNN-short-kebab-title.md` using the next
   free zero-padded number.
2. Fill **Context → Decision → Consequences → Alternatives**. Set status
   `Proposed` (or `Accepted` if it merely records an already-settled decision).
3. Add a row to the [index](#index) below.
4. On acceptance, set status `Accepted` and the date; reference it from
   `README.md` if it changes the roadmap/architecture.
5. To change an accepted decision: write a **new** ADR, set the old one to
   `Superseded by ADR-XXXX`, and cross-link both.

> **Living-docs tie-in:** creating/superseding an ADR is part of the
> document-maintenance protocol in `CLAUDE.md` §9 — keep this index and the
> README's ADR list current in the same change.

## Conventions

- **Filename:** `NNNN-short-kebab-title.md` (4-digit zero-padded, monotonic).
- **Numbers are never reused**, even if an ADR is later superseded.
- One decision per ADR. Keep it short (a page); link to `/docs` for depth.
- Date format `YYYY-MM-DD`.

## Index

| # | Title | Status | Date |
|---|---|---|---|
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions (use ADRs) | Accepted | 2026-06-28 |
| [0002](0002-cpp-realtime-core-python-ml-layer.md) | C++ real-time core + Python research/ML layer | Accepted | 2026-06-28 |
| [0003](0003-hybrid-graph-one-ir-node-contract.md) | Hybrid signal graph: one typed IR, many backends, universal node contract | Accepted | 2026-06-28 |
| [0004](0004-realtime-safety-audio-thread.md) | Real-time safety: the audio thread is sacred | Accepted | 2026-06-28 |
| [0005](0005-io-duplex-callback-swappable-clock.md) | I/O foundation: one duplex callback, swappable clock | Accepted | 2026-06-28 |
| [0006](0006-runtime-agnostic-neural-inference.md) | Runtime-agnostic neural inference (inline RTNeural + off-thread ANIRA) | Accepted | 2026-06-28 |
| [0007](0007-macos-coreaudio-io.md) | macOS audio I/O via Core Audio (HAL + process taps) | Accepted | 2026-06-28 |
| [0008](0008-multi-input-and-full-duplex-clocking.md) | Multi-input & full-duplex clocking (shared clock, aggregate devices, per-source ring buffers) | Accepted | 2026-06-28 |
| [0009](0009-graph-spine-architecture.md) | Graph spine — IR, node model, and eager executor | Accepted | 2026-06-28 |
| [0010](0010-python-control-plane.md) | Python control plane — lock-free command queue, atomic telemetry, RT backend as control-only frontend | Accepted | 2026-06-29 |
| [0011](0011-multi-stream-executor.md) | Multi-stream executor — N input/output streams via source/sink binding (extends ADR-0009) | Accepted | 2026-06-29 |

### Backlog (decisions to record when made)
- Agent control-plane design (LLM-orchestrates-structure + differentiable param tuning) — currently *research direction* in `docs/40-*`, promote to an ADR when locked (it will build on ADR-0010's queue + atomic-swap hooks).
- Plugin format priority (VST3 vs CLAP vs AU) for first release.
- Open-source **license** choice.
- Graph IR serialization format.
- **Per-port channel counts** (G8) — extends ADR-0009's uniform-channel assumption. *(Plan: `docs/76` Phase A.)*
- **Node/edge latency reporting + delay compensation** (G9). *(Plan: `docs/76` Phase A.)*
- **Multi-source manager clocking** (M10) — N backends + N rings + master clock; makes ADR-0008 §5 concrete. *(Plan: `docs/76` Phase C.)*
- *(G10 multi-stream executor: now **[ADR-0011](0011-multi-stream-executor.md)**, Accepted.)*

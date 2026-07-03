# ADR-0019: Visual workbench — localhost server + browser control frontend

- **Status:** Accepted
- **Date:** 2026-07-03
- **Deciders:** Project owner + Claude Code
- **Related:** [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) (Phase 2 roadmap),
  ADR-0002, ADR-0004, ADR-0010, ADR-0020, ADR-0021

## Context

Phase 2 adds a **visual workbench**: the graph must be *seen and edited in a browser* — nodes,
ports, edges, live parameter values, metering — and edited by hand (add/remove/connect/param).
The engine is a C++ real-time core with a Python research/control layer (ADR-0002); control already
flows through Python via a lock-free command queue + atomic telemetry, with the RT backend treated
as a control-only frontend (ADR-0010). Live editing (values + topology via RCU recompile) already
exists (ADR-0010, `docs/cookbooks/83`). Locked scope (`docs/pipeline/85` §3): **local desktop**,
single-user; **React + React Flow** for the editor.

Forces:
- **Reuse, don't re-invent live editing** — the browser should *drive* the existing safe
  mechanisms, not add a new mutation path.
- **The audio thread is sacred** (ADR-0004) — the browser/server must never run audio or block it.
- **One authoritative graph state** — the browser is a *view + control surface*, not a second
  source of truth.

## Decision

**We will build a local-desktop web workbench: a localhost Python server (FastAPI + WebSocket) that
hosts the engine via the nanobind bindings, and a React + React Flow browser frontend that is a
control frontend only (extending ADR-0010).**

1. The **server** exposes graph state, applies **graph-edit actions** (ADR-0020), and streams
   **throttled/downsampled telemetry** (meters/taps/xrun via the existing lock-free rings) over a
   WebSocket. It hosts one engine; single-user; no accounts/auth (local only).
2. The **browser** (React + React Flow, TypeScript/Vite) renders the manifest-driven graph
   (ADR-0021), and sends edits back as actions. It **never** touches audio; the audio thread never
   blocks on the WebSocket.
3. The **engine holds the authoritative graph**; the browser reconciles against server-pushed diffs
   (optimistic UI). Param edits ride the lock-free queue; topology edits use RCU recompile
   (ADR-0010).

## Consequences

**Positive**
- Visual observability + hand-editing over the *existing* live-edit machinery — no new RT path.
- The browser cannot diverge from reality: state and the node palette come from the engine
  (ADR-0021), not hardcoded.
- Foundation for the agent companion (ADR-0022), which shares the same server + action space.

**Negative / costs**
- A new frontend stack (React/TS/Vite) + a Python web server (FastAPI/uvicorn/websockets) enter the
  project — build, packaging, and a WS state-sync problem to solve.

**Neutral / follow-ups**
- Milestones A2 (server), B0–B2 (UI). Requires Graph↔JSON serialization incl. UI layout (ADR-0020).
- Telemetry is throttled; the audio thread never awaits the WS (invariant preserved).

## Alternatives considered

- **Native desktop UI** (Qt/JUCE editor) — heavier, less shareable, duplicates web tooling; the
  browser is the more portable, inspectable surface for a graph editor.
- **Hosted / multi-user web app** — deferred; a Phase-2 non-goal (`docs/pipeline/85` §12): brings
  auth, multi-tenancy, per-user engine isolation. Local-desktop first.
- **TUI / no GUI** — insufficient for a spatial node graph.

## References
- [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) §1–4.
- ADR-0002 (C++ core + Python layer), ADR-0004 (sacred audio thread), ADR-0010 (control plane),
  ADR-0020 (action space), ADR-0021 (capability manifest).

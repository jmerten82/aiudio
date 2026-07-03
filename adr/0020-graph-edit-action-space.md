# ADR-0020: Graph-edit action space & protocol — the shared edit substrate

- **Status:** Accepted
- **Date:** 2026-07-03
- **Deciders:** Project owner + Claude Code
- **Related:** [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md),
  ADR-0009, ADR-0010, ADR-0019, ADR-0021, ADR-0022

## Context

In Phase 2 the graph is mutated from three places: the **hand editor** (browser), the **agent**
(NL → edits), and the **API/wire protocol** (browser ↔ engine). If each has its own edit
representation they will drift, and there is no shared history for undo or for human+agent
collaboration on one graph. The engine already applies parameter changes via a lock-free command
queue and topology changes via an off-thread RCU recompile + atomic swap (ADR-0010); the graph IR is
the single source of truth (ADR-0009).

## Decision

**We will define one typed graph-edit action space as the single substrate for the UI's edit
operations, the agent's tools, the browser↔engine wire protocol, and an append-only action log.**

1. **Actions:** `add_node(type, params)` · `remove_node(id)` · `connect(src,srcPort,dst,dstPort)` ·
   `disconnect(...)` · `set_param(node,index,value)` · `replace_subgraph(...)` · plus read/query
   (inspect graph, read metrics). Actions map directly onto `Graph` + `GraphExecutor`.
2. **Append-only action log** → undo/redo, replay, auditability, and *one shared history* for the
   human editor and the agent editing the same graph.
3. **Validated before apply** against the capability manifest (ADR-0021) — illegal types, params
   out of range, channel/latency mismatches are rejected at the boundary.
4. **Routing by kind:** `set_param` → the lock-free queue (click-free, no recompile); topology
   actions → RCU recompile + atomic swap (ADR-0010). RT-invasive actions additionally gate on
   consent (ADR-0022).
5. **Graph↔JSON serialization** (nodes/edges/params + UI layout metadata) is defined here — the
   persisted/transported form of a graph.

## Consequences

**Positive**
- One source of truth for edits → UI, agent, and API cannot diverge; undo/replay/audit come for free.
- Human + agent collaborate on one graph through one log.
- Also settles the long-open "graph IR serialization format" question (adr backlog).

**Negative / costs**
- A protocol + versioned action log + JSON (de)serialization to design and maintain.

**Neutral / follow-ups**
- Milestone A0. The action log underpins the agent's human-in-the-loop UX (ADR-0022) and undo in B1.

## Alternatives considered

- **Separate UI ops vs. agent tools vs. wire protocol** — three representations that drift; the
  classic maintenance tax. Rejected.
- **Browser calls bindings directly** — no validation, no undo, no shared log, and couples the UI
  to the C++ API surface.

## References
- [`docs/pipeline/85`](../docs/pipeline/85-phase2-agent-workbench-roadmap.md) §4 (A0).
- ADR-0009 (one IR + node contract), ADR-0010 (queue + RCU recompile), ADR-0021 (manifest/validation),
  ADR-0019 (the transport), ADR-0022 (consent for invasive actions).

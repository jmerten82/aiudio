# 85 — Phase 2: Agent Control Plane & Visual Workbench — Roadmap

> **Last updated:** 2026-07-02 · **Goal:** turn the live engine into a **workbench** — a graph you
> can *see and edit in a browser*, *shape by natural language* through a grounded LLM companion, and
> that the agent can *extend itself* by authoring new nodes on the fly. All three surfaces (visual
> editor, agent, self-extension) drive **one live engine** through **one typed action space**,
> grounded in **one capability manifest**. Builds directly on Phase-0 live editing (G7: lock-free
> `set_param` + RCU recompile) and Phase-1's differentiable layer (`match_target`). · **Status:**
> 📋 **planned** — Phase 1 (D0–D8) complete; this is the Phase-2 design. Locked scope decisions
> below. The audio-thread invariant (ADR-0004) is **never** relaxed — the self-extension path
> *enforces* it on generated code (workstream D), and **no invasive change to the live RT audio
> thread is ever applied without active user notification + explicit confirmation** (§5.1a).

---

## Contents
- [1. The idea](#1-the-idea)
- [2. Where it sits](#2-where-it-sits)
- [3. Locked scope (this phase)](#3-locked-scope)
- [4. Architecture](#4-architecture)
- [5. The hard problems + mitigations](#5-the-hard-problems)
- [6. Milestones](#6-milestones)
- [7. Dependency / sequencing graph](#7-dependency-graph)
- [8. Shipping roadmap (releases)](#8-shipping-roadmap)
- [9. New ADRs](#9-new-adrs)
- [10. Definition of done](#10-definition-of-done)
- [11. Risks](#11-risks)
- [12. Non-goals for Phase 2](#12-non-goals)

---

## 1. The idea

Today the graph is driven by code. Phase 2 gives it **three human/agent-facing surfaces over the
same running engine**:

1. **A visual workbench** (browser) — see the full graph: nodes, ports, edges, live parameter
   values, metering, latency. Edit it *by hand*: add/remove nodes, connect/disconnect, change
   parameters — reflected live.
2. **An agent companion** (chat window, same page) — design and change the graph in **natural
   language**, deeply **grounded** in what the pipeline can actually do (real node types, params,
   ranges, RT/differentiability status — introspected, never hallucinated).
3. **Self-extension** — when the existing library can't express something, the agent **authors a
   new node**, which lands in a **personal, local registry** (not shipping `main`), is validated by
   an **RT-safety gate**, and — once built — is **reusable** in future sessions.

The unification is the point: **one typed action space** (`add_node`/`connect`/`set_param`/…) is
simultaneously the *UI's edit operations*, the *agent's tools*, the *browser↔engine wire protocol*,
and an *append-only action log* (undo/redo/replay, and human+agent collaboration on one graph). And
**one capability manifest**, generated from the real node registry, grounds both the UI palette and
the agent.

*The original Phase-2 scope is retained in full* (typed action space; LLM orchestrates structure
while the differentiable layer tunes parameters; render→measure→self-correct) — the visual layer and
self-extension are **additive**.

## 2. Where it sits

Phase 2 is a **control/UX + self-extension layer over machinery that already exists**:

- **Phase 0 (G7 live edit).** The engine already applies parameter changes via a lock-free command
  queue and topology changes via an off-thread **RCU recompile + atomic swap** (`docs/cookbooks/83`,
  ADR-0010). Phase 2 *drives* this from the UI and the agent — it does not re-invent live editing.
- **Phase 1 (differentiable core).** `match_target` / gradient descent (`docs/cookbooks/84`) is how the agent
  **tunes parameters** once it has chosen structure — the "LLM orchestrates structure; the
  differentiable layer tunes parameters" split.
- **Invariants preserved.** Python/JS is a *control frontend* only (ADR-0002); the **audio thread
  stays sacred** (ADR-0004) — enforced on *authored* nodes by the workstream-D gate; live edits use
  the existing safe mechanisms (ADR-0010).

## 3. Locked scope

Decisions taken at kickoff (2026-07-02):

| Decision | Choice | Consequence |
|---|---|---|
| **New-node RT scope** | **Full real-time from day one** | Agent-authored nodes compile to C++ and can run on the audio thread — *only* behind the mandatory RT-safety gate (D2). Highest power; the gate is the guardrail. |
| **Web UI stack** | **React + React Flow** (TypeScript, Vite) | Mature node-editor (ports/edges, pan-zoom, minimap, inspector); large ecosystem. |
| **Personal nodes** | **Local node-package registry** | A git-ignored local plugin dir the engine auto-loads; reusable across sessions; explicit *promote → PR to `main`* path. |
| **Deployment** | **Local desktop (localhost)** | Browser ↔ localhost Python server hosting the engine; single-user; personal nodes + data stay local. No multi-tenant auth/infra in Phase 2. |

Retained from the original Phase 2: the typed graph-edit action space; structure-by-LLM +
params-by-gradient; the render→measure(CLAP/loudness/spectral)→self-correct loop.

## 4. Architecture

```
┌───────────────────────── Browser (React + React Flow) ─────────────────────────┐
│  Graph canvas (nodes/ports/edges, live params + metering)   │  Agent companion  │
│  Hand editing ─┐                                            │  (NL chat)  ─┐    │
└────────────────┼────────────────────────────────────────────────────────┼──────┘
                 │            WebSocket (state ⇄ actions ⇄ telemetry)       │
                 ▼                                                          ▼
┌──────────────── Localhost server (FastAPI + WS, Python) ────────────────────────┐
│  Action space  ── validate vs ─▶  Capability manifest  ◀── introspect ── registry│
│  + action log       │                                                            │
│  (undo/replay)      ▼                                     Agent runtime (Claude) │
│              aiudio Python bindings                        tools = action space  │
│              (Graph, GraphExecutor: set_param queue / RCU recompile — G7)        │
│                     │                         Node authoring ▶ RT-safety gate ▶  │
│                     ▼                         (D) ▶ local node registry (reuse)  │
│              C++ real-time engine  ◀── dlopen authored node plugins (off-thread) │
│              (audio thread — sacred, ADR-0004)                                   │
└──────────────────────────────────────────────────────────────────────────────┘
                 │ diff layer (Phase 1): match_target tunes params
                 ▼ measure: CLAP / LUFS / spectral  ─▶ self-correct loop
```

**Key components**
- **Action space + log (A0).** `add_node(type, params) · remove_node · connect(src,dst,ports) ·
  disconnect · set_param(node,index,value) · replace_subgraph · query/inspect`. Append-only log →
  undo/redo, replay, and one shared history for human + agent. Maps onto `Graph`+`GraphExecutor`.
- **Capability manifest (A1).** JSON generated from the registry: every node type, its params
  (index, name, range, default, unit), ports/channel behavior, and metadata (`realtime_capable`,
  differentiability, latency, causal). The **single grounding source** for UI editors *and* the
  agent — no hardcoded, drift-prone catalog.
- **Server bridge (A2).** FastAPI + WebSocket hosting the engine via the bindings; streams graph
  state + telemetry (meters/taps/xrun via the existing lock-free rings), applies actions.
- **Web UI (B).** React Flow renders the manifest-driven graph; hand editing round-trips through the
  action space.
- **Agent (C).** Claude tool-use bound to the action space, system-grounded by the manifest;
  companion chat; the measure→self-correct loop + hand-off to `match_target`.
- **Node authoring + registry + gate (D).** Scaffold → compile → **RT-safety gate** → local
  registry → dynamic load (off-thread, via RCU). Full-RT, gated.

## 5. The hard problems

1. **Agent-authored C++ on the audio thread** *("full RT from day one")*. **Isolation is the
   primary safety property, not a gate:** authored nodes live in the **local registry**, **never
   auto-merge to `main`**, and are **disposable** — if one is bad, delete it; nothing leaks into the
   shipping product or other users. The residual risk is therefore narrow and *recoverable*: a node
   that allocates/locks in `process()` glitches or crashes **your own local audio session** (restart
   → gone). So D2 is a **cheap automatic pre-flight**, not a fortress — its job is to catch the
   obvious RT violations *before* the node hits the live audio thread (so a bad node isn't
   discovered as a mid-session dropout), and its failure path *is* "throw it away":
   (a) **static analysis** of `process()` — reject forbidden calls (alloc/locks/syscalls/logging/
   exceptions/unbounded loops); (b) the existing **sanitizers** (ASan/UBSan/TSan) + a **real-time-
   safety check** (RTSan / allocation hook on `process()`); (c) **golden-render + contract tests**.
   Fail → **auto-discard, or quarantine to the non-RT executors** (throwing away broken code needs
   no approval). ADR-0004 is never relaxed — the pre-flight *enforces* it on generated code before
   RT-load; provenance + one-click rollback throughout.

   **Three distinct gates, by concern — don't conflate them:**
   - **Automatic pre-flight** *(code correctness)* — the D2 checks above; fail ⇒ silent discard.
   - **Inform + confirm** *(committing an invasive change to the live audio thread)* — see §5.1a.
   - **Heavyweight review** *(shipping)* — at **promote-to-`main`** (the PR — the only path that
     affects the product or other users).

**5.1a — RT-invasive changes require active notification + explicit confirmation.** Even code that
passed pre-flight must never be pushed onto the **sacred RT audio thread or its components** without
the user being *told what is about to happen* and *actively approving it*. The workbench (UI + agent)
**must**: (1) **surface**, before applying, a clear description of the impending change and its
impact — which node/subgraph, that it is (or contains) **agent-authored native code**, the latency/
PDC delta, and any xrun/dropout risk; and (2) require an **explicit user confirmation** — the agent
may *propose* and stage an invasive change but **cannot commit it to the RT thread on its own**.

*RT-invasive* (⇒ inform + confirm) vs. *routine* (⇒ applies live, no prompt):

| Invasive — needs consent | Routine — no prompt |
|---|---|
| Loading/running an **agent-authored node** on the audio thread | `set_param` on an existing node (click-free, bounded — G7) |
| Inserting a node that **changes latency / PDC** of a live output path | Automation/ramps within a node's declared range |
| Changing **sample rate / block size / device / clock** of the running engine | Reading back state / metering |
| Replacing/removing a node or **subgraph in a live path carrying audio to hardware** | Editing a graph that is **not** currently driving output |

Default posture: **auto-apply routine edits** (fast, reversible via the action log); **stage-and-
confirm invasive ones**. This is enforced in the agent runtime + UI (ADR-0022) — orthogonal to the
throw-away pre-flight and the promote-to-`main` review.
2. **Dynamic loading into a running RT engine.** *Mitigation:* a stable **node-plugin ABI**; `dlopen`
   + factory registration happen **off the audio thread**; insertion uses the existing **RCU
   recompile + atomic swap**; versioning + unload safety.
3. **Agent reliability / grounding drift.** *Mitigation:* the manifest is generated from the real
   registry (single source of truth) → no hallucinated nodes; **every action validated** against it
   before apply; **active inform + confirm for RT-invasive changes** (§5.1a); the **action log**
   gives undo + replay + auditability.
4. **Local code-execution security** (agent compiles + loads code locally). *Mitigation:* sandboxed
   build; **explicit user confirmation before RT-load** (§5.1a — loading authored code is invasive);
   provenance; a policy that generated nodes take no network/IO; local-only, single-user.
5. **State sync (UI ⇄ engine ⇄ agent).** *Mitigation:* the engine holds the **authoritative** graph;
   the action log is the source of truth; the WS pushes diffs; optimistic UI with reconciliation.
6. **Web-layer performance vs. the audio thread.** *Mitigation:* telemetry throttled/downsampled;
   the audio thread **never** blocks on the WS; meters/taps via the existing lock-free ring buffers.

## 6. Milestones

Four workstreams (A platform · B UI · C agent · D self-extension) + a kickoff. `⬜ planned`.

| # | Milestone | What | Depends |
|---|---|---|---|
| **K** ⬜ | Kickoff — scope, ADRs, vision | Lock scope in `docs/00` + README; write ADRs 0019–0024 (§9). | Phase 1 |
| **A0** ⬜ | Action space + IR (de)serialization | The typed edit ops + append-only action log (undo/redo/replay); `Graph`↔JSON (+ UI layout). Over G7. | K |
| **A1** ⬜ | Capability manifest | Extend node introspection (param name/range/default/unit + ports + metadata); emit JSON manifest — the grounding SoT. | A0 |
| **A2** ⬜ | Localhost server bridge | FastAPI + WebSocket over the bindings: state, actions, live telemetry; session/reconnect. | A1 |
| **B0** ⬜ | Read-only graph view | React Flow app: render graph from JSON; live params + metering; node inspector. | A2 |
| **B1** ⬜ | Hand editing | Add (manifest palette)/remove/connect/disconnect/param-edit via the action space → live recompile/`set_param`; undo/redo; connection validation. | B0 |
| **B2** ⬜ | Workbench UX | Layout persistence, subgraph grouping, save/load a graph (JSON), metering/PDC visualizations, error surfaces. | B1 |
| **C0** ⬜ | Grounded agent tools | Claude tool-use bound to the action space; system context from the manifest (A1); read-back/inspect tools. | A1, A0 |
| **C1** ⬜ | NL companion | Chat window: NL → proposed actions → preview/apply; shared action log with the hand editor; explanations. **Routine edits auto-apply; RT-invasive changes are staged and require active notification + explicit confirmation (§5.1a / ADR-0022).** | C0, B1 |
| **C2** ⬜ | Measure → self-correct + diff tuning | render → measure (CLAP/LUFS/spectral) → self-correct; **structure by LLM, params by `match_target`** (Phase 1). *Completes the original Phase-2 vision.* | C1, Phase 1 |
| **D0** ⬜ | Node-package format + local registry | Package (manifest + C++ src + tests + registration); git-ignored local dir, auto-discovered/loaded; versioning; *promote → PR* path. | A1 |
| **D1** ⬜ | Scaffold + build pipeline | Spec → Node-contract C++ (templates) + tests + bindings → compile to a loadable plugin; off-thread build; load via RCU. | D0 |
| **D2** ⬜ | RT-safety pre-flight | An **automatic** pre-flight (§5.1): static checks + sanitizers/RTSan + golden/contract tests, run before RT-load. Pass → usable (incl. RT); fail → **auto-discard / quarantine to non-RT** (no human ceremony for local use — throw-away *is* the failure path). Enforces ADR-0004 on generated code. | D1 |
| **D3** ⬜ | Agent authors nodes end-to-end | Detect capability gap → author (spec→C++→tests) → gate → register locally → use in the graph; provenance/rollback; reusable next session. | D2, C1 |

## 7. Dependency graph

```
K ─▶ A0 ─▶ A1 ─▶ A2 ─▶ B0 ─▶ B1 ─▶ B2
          │        └────────────▶ C0 ─▶ C1 ─▶ C2   (+ Phase 1 diff)
          └─▶ D0 ─▶ D1 ─▶ D2 ─▶ D3   (D3 also needs C1)
```

A1 (the manifest) is the hinge — UI, agent, and node-authoring all depend on it. C2 needs the
Phase-1 diff layer. D2 (the gate) is a hard prerequisite for any RT-loading in D3.

## 8. Shipping roadmap

Milestones bundle into five usable releases:

| Release | Theme | Bundles | You can… |
|---|---|---|---|
| **R1** | **See it** | K · A0 · A1 · A2 · B0 | watch a live graph in the browser — nodes, edges, params, metering. |
| **R2** | **Edit it by hand** | B1 · B2 | build/modify graphs visually: add/remove/connect/tune, undo, save/load. |
| **R3** | **Talk to it** | C0 · C1 | change the graph by natural language via the grounded agent companion. |
| **R4** | **It tunes itself** | C2 | agent runs render→measure→self-correct and tunes params via the diff layer *(original Phase-2 vision complete)*. |
| **R5** | **It extends itself** | D0 · D1 · D2 · D3 | agent authors new (full-RT, gated) nodes into your local registry, reusable thereafter. |

Rationale: observability (R1) before editing (R2) before automation (R3→R4); **self-extension (R5)
ships last** — it's the riskiest and depends on the manifest, action space, and agent being solid.

## 9. New ADRs

To write at kickoff (K) — Phase 2 adds load-bearing, hard-to-reverse decisions (§10 CLAUDE.md):

- **ADR-0019 — Visual workbench architecture.** Localhost server + browser control frontend (React
  Flow + FastAPI/WebSocket). Extends ADR-0002/0010; the browser is a control frontend, never RT.
- **ADR-0020 — Graph-edit action space & protocol.** The shared edit ops + wire protocol + append-
  only action log (undo/replay); the substrate for UI + agent + API.
- **ADR-0021 — Capability manifest as grounding source of truth.** Node/param introspection schema;
  generated from the registry; consumed by UI + agent.
- **ADR-0022 — Agent runtime & human-in-the-loop policy.** Claude tool-use; validation-against-
  manifest; and the **RT-invasive consent rule** (§5.1a) — the agent may propose/stage but **cannot
  commit a change to the live audio thread or its RT-critical components without active user
  notification + explicit confirmation**; routine `set_param`/automation auto-applies. Defines the
  invasive-vs-routine classification and the surfaced-impact contract.
- **ADR-0023 — Personal node registry & isolation.** Local git-ignored plugin dir; package format;
  reuse; *promote → PR to `main`*.
- **ADR-0024 — RT-safety gate & node-plugin loading ABI.** Enforces ADR-0004 on authored code
  (static + RTSan + tests + quarantine + confirm); off-thread `dlopen` + RCU insertion.

## 10. Definition of done

Phase 2 is "the agent workbench is built" when:

1. **A running graph is visible + hand-editable in the browser** — add/remove/connect/param, live,
   with undo and save/load (R1–R2).
2. **The agent edits the graph from natural language**, grounded in the real manifest, human-in-the-
   loop, sharing one action log with the hand editor (R3).
3. **The closed loop works** — render → measure (CLAP/loudness/spectral) → self-correct, with
   structure chosen by the LLM and parameters tuned by the differentiable layer (R4).
4. **The agent can author a new node on the fly** — it passes the RT-safety pre-flight, lands in the
   local registry, and is reusable in a later session; the audio-thread invariant holds throughout
   (R5).
5. **No invasive RT change is ever applied silently** — every change that touches the live audio
   thread or its RT-critical components (§5.1a) is surfaced with its impact and gated on **explicit
   user confirmation**; routine edits auto-apply. Enforced across R3–R5 (ADR-0022).
6. **Docs/ADRs current** — `docs/00` scope + README Phase-2 updated; ADRs 0019–0024 accepted; a
   workbench cookbook (the fifth in the `docs/cookbooks/81–84` series) added.

**Explicit non-goals for Phase 2** — see §12.

## 11. Risks

- **RT correctness of generated code** *(bounded, recoverable)* — isolation removes the shipping/
  supply-chain risk (local registry, no auto-merge, disposable); the residual is a glitch/crash of
  *your own local session*, caught before RT-load by the D2 automatic pre-flight and fail-safed by
  discard/quarantine. Not a blocker — the review that matters is at promote-to-`main`.
- **Agent produces plausible-but-wrong graphs/nodes** — validation vs. manifest, human-in-the-loop,
  undo/replay, contract + golden tests on authored nodes.
- **Scope is large** — the release ladder (R1→R5) delivers value incrementally; each release is
  independently useful, and R5 (the risky bit) is isolated at the end.
- **Local build/toolchain friction** (compiling authored nodes on the user's machine) — a scaffold +
  pinned toolchain + clear errors; fall back to the non-RT quarantine when RT build/gate fails.

## 12. Non-goals

Deferred beyond Phase 2 (kept honest): multi-user/hosted deployment + accounts (Phase 2 is
local/single-user); a full **DAW timeline/arrangement UI** (still a non-goal, `docs/00` §4 — this is
a *signal-graph* editor, not a linear arranger); the neural-model *zoo* (separation/codecs/
generation — Phase 4); RT neural *inference* runtime (RTNeural/ANIRA on the audio thread — Phase 3,
ADR-0006); mobile/embedded targets. Auto-upstreaming personal nodes is out — promotion to `main` is
an explicit, reviewed PR.

---

### Cross-references
- **Builds on:** [`docs/pipeline/79`](79-phase1-differentiable-core-roadmap.md) (Phase 1) ·
  [`docs/cookbooks/84`](../cookbooks/84-differentiable-and-trainable-graphs.md) (diff workflow) ·
  [`docs/cookbooks/83`](../cookbooks/83-live-control-and-dynamic-graphs.md) (live edit / RCU) ·
  [`docs/pipeline/78`](78-node-library-roadmap.md) (node library).
- **Vision & research:** [`docs/00`](../00-vision-and-scope.md) (scope) ·
  [`docs/theory/40`](../theory/40-ai-agents-for-audio.md) (agents) · [`docs/theory/50`](../theory/50-architecture-patterns.md) (graph engine).
- **Why (ADRs):** [0002](../../adr/0002-cpp-realtime-core-python-ml-layer.md) ·
  [0004](../../adr/0004-realtime-safety-audio-thread.md) · [0010](../../adr/0010-python-control-plane.md) ·
  plus 0019–0024 (this phase, §9).

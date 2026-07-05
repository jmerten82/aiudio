# 87 — Phase 2 Testing Plan (agent control plane & visual workbench)

> **Last updated:** 2026-07-05 · **Scope:** how the Phase-2 workbench is tested — the action space,
> capability manifest, localhost server, browser UI, grounded agent, and differentiable self-tuning.
> Extends the Phase-0 strategy ([`testing/README.md`](../../testing/README.md)); the audio-thread
> invariants + C++ core are covered there. Phase 2 adds **Python + TypeScript** layers and a
> **client/server contract**, so the plan centers on *headless* verification of both sides.

---

## 1. Philosophy

Four principles, all a consequence of the frontend/backend separation (ADR-0019):

1. **Headless-first.** Every layer is testable without a browser, an audio device, or an API key —
   the meaningful logic lives in pure functions + a server reachable by an in-process ASGI client.
2. **Contract tested on *both* sides.** The graph-edit action space (ADR-0020) is the wire contract;
   the TS builders (`web/src/actions.test.ts`) and the Python parser/apply (`test_workbench_*`) are
   tested against the *same* op/field shapes, so drift is caught on either end.
3. **Grounding is verified, not assumed.** The manifest (ADR-0021) is introspected from the real
   registry and its descriptors are cross-checked against actual node defaults, so the UI/agent
   can't drift from — or hallucinate beyond — what the pipeline offers.
4. **Optional layers gate, never fail.** torch (`[diff]`), FastAPI (`[workbench]`), and Anthropic
   (`[agent]`) are optional; their tests `importorskip` and CI installs the extras so they *run*.

## 2. What's tested, where

| Layer | Suite | Covers |
|---|---|---|
| **A0 action space + session** | `testing/python/test_workbench_actions.py` | action ↔ dict round-trips (incl. `set_position`); apply builds a real `Graph`; unknown-kind / bad-connect raise; graph↔JSON document round-trip (topology + params + layout); `parametric_eq` bands survive; action-log replay; undo/redo (+ redo clears); remove drops node + edges; position persists + round-trips |
| **A1 manifest + validation** | `test_workbench_manifest.py` | manifest covers the palette; **every descriptor self-consistent** (min ≤ default ≤ max); **descriptor defaults == real node defaults** (compile-checked); ports + RT-capability; variable-arity params (mixer); `param_issues`; session rejects an undeclared param index; `defaults` let you add any (required-arg) kind |
| **A2/C1 server** | `test_workbench_server.py` | HTTP `/api/health`/`/manifest`/`/graph`; WS initial state; apply→ack+broadcast; undo; bad action errors without mutating; **two clients share one graph**; static SPA serving; **chat runs the agent (mock client) → applies + broadcasts**; chat-without-agent → graceful error; **`tune` optimizes + broadcasts** |
| **C0 agent** | `test_agent.py` | tools grounded in the manifest (kind enum == real kinds); system prompt lists real nodes/params; `apply_tool` maps to the session; the tool-use loop builds a graph from scripted calls; **tool errors reported, not raised**; **consent gate** declines/allows invasive changes |
| **C2 tuning** | `test_workbench_tuning.py` | `tune_to_target` recovers a gain + **writes back** to the session; MSE variant; iterative-idempotent |
| **Frontend logic** | `web/src/graph.test.ts` | `documentToFlow` (nodes/edges/handles/labels/layout); `reconcile` (preserve local positions, add/drop); `handlePort` |
| **Frontend contract** | `web/src/actions.test.ts` | action builders emit exactly the Python-parsed dicts (op + field names); `set_position`, `load`, `chat` shapes |
| **Frontend type/build** | CI `web` job | `tsc --noEmit` + `vite build` (196+ modules) — the whole app type-checks and bundles |

## 3. CI matrix

`.github/workflows/ci.yml` runs on every push/PR:
- **cpp** (ubuntu + macos) + **sanitizers** (TSan, ASan/UBSan) — the Phase-0 RT core (unchanged).
- **python** (macos) — `pip install ".[diff,workbench]"` then `pytest testing/python bindings` +
  `ruff` — so the workbench/server/tuning/agent tests run rather than skip.
- **web** (ubuntu) — `npm ci` → `typecheck` → `vitest` → `build`.

Run locally: `pytest testing/python -q` · `ruff check python bindings examples testing` ·
`cd web && npm run typecheck && npm test && npm run build`.

## 4. Gated / manual tests

Some behavior needs a resource CI doesn't have; these **skip** by default and are run by hand:

| Test | Gate | How to run |
|---|---|---|
| Live agent (real Claude edits a graph) | `ANTHROPIC_API_KEY` | `ANTHROPIC_API_KEY=… pytest testing/python/test_agent.py -k live` |
| Live audio device (RT playback/capture) | `AIUDIO_LIVE=1` on a mac w/ a device | Phase-0 liveness job |
| Browser end-to-end (manual QA) | a human | §5 checklist |

## 5. Manual QA checklist (the human end-to-end pass)

Run the backend + the dev UI, then walk the workbench:

```bash
pip install -e ".[workbench,diff,agent]"
python -m aiudio.server              # terminal 1  (localhost:8765)
cd web && npm install && npm run dev # terminal 2  → open the printed URL
```

- [ ] The canvas loads; the palette lists all node kinds; status shows **connected**.
- [ ] **Add** source → gain → sink from the palette; **connect** them by dragging handles.
- [ ] **Select** the gain; the inspector shows its params; drag the slider → the value updates live.
- [ ] **Delete** a node/edge; **Undo**/**Redo** step correctly.
- [ ] **Drag** a node; **Save** → a JSON downloads; reload the page + **Load** → layout + graph restored.
- [ ] Open a second browser tab → edits in one appear in the other (shared authoritative graph).
- [ ] **Chat** (needs `ANTHROPIC_API_KEY` on the server): "add a compressor after the gain" → the
      graph changes + the agent replies; an impossible request → a sensible refusal.
- [ ] `python -m aiudio.server --static web/dist` (after `npm run build`) serves the whole app.

## 6. Known gaps + proposed additions

Honest about what isn't covered yet, and how to close it:

- **No browser E2E automation.** The pure logic + server-contract tests cover the *contract*, but a
  click-through isn't automated. → Add a **Playwright** smoke test (load → add via palette → assert
  a node renders → connect → assert the edge) against `npm run dev` + a test server.
- **No component-render tests.** `documentToFlow`/`reconcile` are unit-tested, but React components
  (Inspector/Chat/GraphView) aren't. → Add **Vitest + jsdom + @testing-library** render tests.
- **RT-invasive consent path unexercised.** Nothing is invasive in an offline session, so the
  `on_invasive` gate (ADR-0022) is unit-tested in isolation but not end-to-end. → Exercise it once a
  **live executor + telemetry** are attached (the confirm-dialog milestone).
- **Live metering untested.** The telemetry channel is a documented follow-up; add tests when it lands.
- **Agent-edit *quality* only via the live test.** The mock proves the plumbing; whether Claude makes
  *good* edits is validated by the `ANTHROPIC_API_KEY`-gated test — run it in a nightly/manual lane.

## 7. Coverage summary

| Feature | Unit | Integration | Contract (both sides) | Gated/manual |
|---|---|---|---|---|
| Action space + serialization | ✅ | ✅ (server WS) | ✅ | — |
| Capability manifest + validation | ✅ | — | — | — |
| Server (HTTP + WS + broadcast) | — | ✅ | — | — |
| Web transforms + builders | ✅ | — | ✅ | — |
| Web UI rendering | — | — | — | ⚠ manual (§5) / E2E gap |
| Agent tool-use + grounding + consent | ✅ | ✅ (chat) | — | live (key) |
| Differentiable tuning | ✅ | ✅ (`tune` WS) | — | — |

---

### Cross-references
- Phase-0 strategy + runner: [`testing/README.md`](../../testing/README.md) / `testing/run.sh`.
- What's being tested: [`docs/pipeline/85`](85-phase2-agent-workbench-roadmap.md) (Phase 2) ·
  the cookbook [`docs/cookbooks/88`](../cookbooks/88-agent-workbench.md).
- ADRs: [0019](../../adr/0019-visual-workbench-architecture.md)/[0020](../../adr/0020-graph-edit-action-space.md)/[0021](../../adr/0021-capability-manifest-grounding.md)/[0022](../../adr/0022-agent-runtime-and-consent-policy.md).

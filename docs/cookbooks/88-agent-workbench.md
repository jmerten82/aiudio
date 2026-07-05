# 88 — The Agent Workbench (cookbook)

> **Last updated:** 2026-07-05 · **Scope:** how to *use* the Phase-2 workbench — build and edit an
> aiudio graph through the **typed action space** (Python), serve it to a **browser editor**, drive
> it with the **grounded LLM companion**, and let it **tune its own parameters**. Grounded in the
> merged Phase-2 code (`aiudio.workbench` / `aiudio.server` / `aiudio.agent` / `web/`, **✓ Verified**).
> The **fifth cookbook**: `docs/cookbooks/81–83` are the RT pipeline; `84` is the differentiable
> layer; this one is the **agent + visual workbench** (ADR-0019/0020/0021/0022).

Optional extras: `pip install "aiudio[workbench]"` (server), `"aiudio[diff]"` (tuning),
`"aiudio[agent]"` (the LLM companion).

---

## Contents
- [0. Run it](#0-run-it)
- [1. The action space (build a graph in Python)](#1-the-action-space)
- [2. The capability manifest (grounding)](#2-the-capability-manifest)
- [3. The server + WebSocket protocol](#3-the-server)
- [4. Editing by hand in the browser](#4-editing-by-hand)
- [5. The agent companion (edit by natural language)](#5-the-agent-companion)
- [6. Self-tuning (params by gradient)](#6-self-tuning)
- [7. Embedding the workbench headlessly](#7-embedding)
- [Appendix — message reference](#appendix)

---

## 0. Run it

```bash
pip install -e ".[workbench,diff,agent]"
export ANTHROPIC_API_KEY=sk-…            # only for the agent companion (§5)
python -m aiudio.server                  # localhost:8765  (terminal 1)

cd web && npm install && npm run dev      # the UI, proxying /api + /ws → :8765 (terminal 2)
```

Open the printed URL: a React Flow canvas, a node palette, an inspector, and an agent chat panel.
For a single self-served process: `npm run build` then `python -m aiudio.server --static web/dist`.

## 1. The action space

Everything — the UI, the agent, the wire protocol — edits the graph through one **typed action
space** (ADR-0020). In Python it's a `GraphSession`:

```python
from aiudio import workbench as wb

s = wb.GraphSession()
src = s.add_node("source")
gain = s.add_node("gain", {"gain": 0.8}, position=(120, 80))   # kind, ctor args, UI layout
comp = s.add_node("compressor", {"threshold_db": -24.0, "ratio": 4.0})
snk = s.add_node("sink")
s.connect(src, 0, gain, 0)
s.connect(gain, 0, comp, 0)
s.connect(comp, 0, snk, 0)

s.set_param(comp, 1, 6.0)      # ratio → 6 (validated against the manifest, §2)
s.undo()                       # append-only log → undo/redo/replay
```

Serialize the whole graph to a **document** (`{nodes, edges}` with args, params, layout) — the form
the browser renders and the wire transports:

```python
doc = s.to_document()                       # or s.to_json()
same = wb.GraphSession.from_document(doc)    # rebuild (topology preserved)
log = s.log_to_list()                        # the action history (for replay)
```

## 2. The capability manifest

The manifest is **introspected from the real node registry** (ADR-0021) — it's what grounds the UI
palette *and* the agent, so neither can invent nodes/params that don't exist:

```python
m = wb.capability_manifest()["kinds"]
m["compressor"]["params"]          # [{index, name, min, max, default, unit}, …]
m["compressor"]["defaults"]        # ctor args to *add* one
m["neural_node"]["realtime_capable"]   # False — a placeholder in RT (Phase 3)

wb.param_issues("compressor", 1, 4.0)   # []  (ratio in range)
wb.param_issues("compressor", 99, 0.0)  # ['compressor has no parameter index 99']
```

`GraphSession(validate=True)` (the default) rejects `set_param` on an undeclared index using these
descriptors.

## 3. The server

`python -m aiudio.server` hosts **one authoritative `GraphSession`** and speaks the action space
over HTTP + WebSocket (ADR-0019). HTTP is read-only:

```bash
curl localhost:8765/api/manifest    # the capability manifest
curl localhost:8765/api/graph       # the current document
```

The `/ws` WebSocket is the live channel. On connect it sends `manifest` then `graph`; you send
**actions** and it **broadcasts** the new graph to *every* client (so hand + agent edits stay in
sync). A minimal Python client:

```python
import asyncio, json, websockets   # pip install websockets

async def main():
    async with websockets.connect("ws://localhost:8765/ws") as ws:
        await ws.recv(); await ws.recv()                       # manifest, initial graph
        await ws.send(json.dumps({"type": "action",
            "action": {"op": "add_node", "node": "gain", "args": {"gain": 0.5}}}))
        print(await ws.recv())    # {"type":"result","value": <new id>}
        print(await ws.recv())    # {"type":"graph","doc": {...}}  (broadcast)
asyncio.run(main())
```

See the [Appendix](#appendix) for every message type.

## 4. Editing by hand

The browser is a **control frontend** — it never runs audio; it renders the broadcast graph and
sends actions back. In the UI you can:
- **Add** a node from the palette (uses the manifest `defaults`), **connect** by dragging port
  handles, **delete** nodes/edges, **drag** to lay out (positions persist via `set_position`).
- **Select** a node → the inspector shows manifest-driven param controls (slider + number, bounded
  by the descriptor range) → editing sends `set_param` (debounced).
- **Undo/Redo**, and **Save**/**Load** a graph as JSON.
Every edit round-trips through the server and re-broadcasts, so a second browser tab (or the agent)
sees it too.

## 5. The agent companion

Type a request in the chat panel and a **grounded** Claude edits the graph. Its tools *are* the
action space and its system prompt is built from the manifest, so it only uses nodes/params that
exist (ADR-0022). Headless equivalent:

```python
from aiudio.agent import Agent, AnthropicClient

s = wb.GraphSession()
agent = Agent(s, AnthropicClient())                 # needs aiudio[agent] + ANTHROPIC_API_KEY
result = agent.run("Add a source, a gain at 0.5, and a compressor; connect them in order.")
print(result.text, "—", len(result.applied), "edits")
print(s.to_document())
```

The agent and the human share one graph + one action log. **RT-invasive** changes (touching the
live audio thread) require an approval callback (ADR-0022 §5.1a) — wired via `on_invasive`; nothing
is invasive in an offline session yet. Testing without a key uses a mock client (see
[`docs/pipeline/87`](../pipeline/87-phase2-testing-plan.md)).

## 6. Self-tuning

The LLM picks *structure*; the **differentiable layer tunes the numbers** (Phase 1 · `match_target`).
`tune_to_target` renders the graph, optimizes its params against a target, and writes them back:

```python
import numpy as np
s = wb.GraphSession()
src, gn, snk = s.add_node("source"), s.add_node("gain", {"gain": 1.0}), s.add_node("sink")
s.connect(src, 0, gn, 0); s.connect(gn, 0, snk, 0)

x = np.random.default_rng(0).standard_normal((1, 512)).astype("float32")
stats = wb.tune_to_target(s, x, x * 0.3)            # needs aiudio[diff]
print(stats)                                        # {'loss_before':…, 'loss_after':…, 'steps':…}
# the gain param has moved toward 0.3, written back into the session document:
print(s.to_document()["nodes"][1]["params"])        # {'0': ~0.3}
```

Over the wire it's a `{"type": "tune", "input": …, "target": …}` message; the server runs it
off-thread and broadcasts the tuned graph.

## 7. Embedding

`GraphSession` is a self-contained, torch-free workbench you can drive from any Python — build a
graph, serialize it, replay a log, tune it — without the server or UI. It's the same object the
server hosts, so anything you script maps directly onto the browser + agent experience.

---

## Appendix — message reference

**Client → server** (over `/ws`):

| Message | Effect |
|---|---|
| `{type:"action", action:{op:"add_node", node, args, position}}` | add a node → `result` (new id) + `graph` |
| `{op:"remove_node", id}` · `{op:"connect"/"disconnect", src, src_port, dst, dst_port}` | edit topology |
| `{op:"set_param", node, index, value}` · `{op:"set_position", id, x, y}` | edit a param / layout |
| `{type:"undo"}` · `{type:"redo"}` · `{type:"sync"}` | history / resend state |
| `{type:"load", doc}` | replace the whole graph from a document |
| `{type:"chat", message}` | run the agent companion (§5) |
| `{type:"tune", input, target, steps?, lr?}` | differentiable tuning (§6) |

**Server → client:** `{type:"manifest", manifest}` · `{type:"graph", doc}` (broadcast) ·
`{type:"result", value}` · `{type:"agent", text, applied}` · `{type:"tuned", loss_before, loss_after}` ·
`{type:"error", message}`.

### Cross-references
- **Roadmap:** [`docs/pipeline/85`](../pipeline/85-phase2-agent-workbench-roadmap.md) · **Testing:** [`docs/pipeline/87`](../pipeline/87-phase2-testing-plan.md).
- **Under the hood:** the differentiable layer [`docs/cookbooks/84`](84-differentiable-and-trainable-graphs.md); live control [`docs/cookbooks/83`](83-live-control-and-dynamic-graphs.md).
- **Why (ADRs):** [0019](../../adr/0019-visual-workbench-architecture.md)/[0020](../../adr/0020-graph-edit-action-space.md)/[0021](../../adr/0021-capability-manifest-grounding.md)/[0022](../../adr/0022-agent-runtime-and-consent-policy.md).

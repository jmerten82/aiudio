# 83 — Live Control & Dynamic Graphs (cookbook)

> **Last updated:** 2026-06-30 · **Scope:** how to *change a running graph* — parameters and
> topology — from the control side, safely, in **C++ and Python**. Grounded in merged code
> (**✓ Verified**). This is the third cookbook: `docs/81` is *topology* (clocks/sources/output),
> `docs/82` is *nodes* (DSP chains), and this one is **control** — the "aiudio is a control
> frontend over a real-time C++ core" story (ADR-0002/0004/0010). Per-function API:
> [`docs/80`](80-pipeline-capabilities.md).

---

## Contents
- [0. The control model — two rules](#0-the-control-model)
- [1. Change a parameter live](#1-change-a-parameter-live)
- [2. Automation from the control thread (ramps, LFOs, envelopes)](#2-automation-from-the-control-thread)
- [3. A control surface — map names → (node, param)](#3-a-control-surface)
- [4. Insert a node into a live chain](#4-insert-a-node-into-a-live-chain)
- [5. Bypass / rewire live](#5-bypass--rewire-live)
- [6. Remove a node safely while running](#6-remove-a-node-safely-while-running)
- [7. Read the graph back (introspection)](#7-read-the-graph-back)
- [8. Close the loop — monitor while controlling](#8-close-the-loop)
- [Appendix A — What's safe from the control thread](#appendix-a--whats-safe-from-the-control-thread)
- [Appendix B — Lifetime & the atomic swap (RCU)](#appendix-b--lifetime--the-atomic-swap)
- [Appendix C — Cross-references](#appendix-c--cross-references)

---

## 0. The control model

Everything here rests on **two rules**.

**Rule 1 — the control thread never touches the audio thread (ADR-0004/0010).** Python (or any
non-RT C++ control code) never calls into a node's `process()`, never mutates a live schedule,
never allocates on the audio path. It communicates through exactly two mechanisms:

| You want to change… | Mechanism | Cost | Recompile? |
|---|---|---|---|
| a **parameter value** (gain, cutoff, ratio, mix, pan…) | `ex.set_param(node, index, value)` → a **lock-free command queue**, applied by the audio thread at the next block | wait-free | **no** |
| the **topology** (add / remove / rewire nodes) | edit the `Graph`, then `ex.compile(g, …)` → builds a new schedule off-thread and **atomically swaps** it in (RCU) | off-thread alloc | **yes** |

**Rule 2 — value changes are click-free; structural changes are atomic.** Continuous params are
internally **smoothed** (`SmoothedValue`), so automating them per block doesn't zipper. A
`compile()` swap is a single atomic pointer exchange — the audio thread runs either the whole old
schedule or the whole new one, never a half-edited graph; the retired schedule is reclaimed
off-thread once the audio thread has moved past it.

So the mental split is: **`set_param` for *values* (cheap, continuous, no recompile); `compile()`
for *structure* (atomic, occasional).** Reach for a recompile only when the node *graph* changes,
not when a knob moves.

> In the examples below the graph is driven synchronously with `ex.process(block)` so each snippet
> is runnable/verifiable stand-alone; in production the same calls happen while a backend's IOProc
> drives `process()` (see `docs/81`) — you just `set_param`/`compile` from your control loop
> instead of between `process()` calls. `import aiudio as a` (Python), `namespace aiudio` (C++).

---

## 1. Change a parameter live

`ex.set_param(node, index, value)` queues a control-rate change applied at the next block. It
returns `False` only if the command queue is momentarily full (see §2). Convenience wrappers:
`set_gain` (a `Gain`), `set_cutoff`/`set_q` (any `Biquad`). Param indices are in
[`docs/82`](82-node-usage-patterns.md#appendix--parameter-index-quick-reference).

**Python**
```python
import numpy as np, aiudio as a
SR = 48000.0
g = a.Graph()
src = g.add_source(); gain = g.add_gain(1.0); lp = g.add_biquad_lowpass(2000.0, 0.707, SR)
snk = g.add_sink()
g.connect(src, 0, gain, 0); g.connect(gain, 0, lp, 0); g.connect(lp, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

ex.set_gain(gain, 0.5)              # convenience: GainNode gain
ex.set_cutoff(lp, 800.0)            # convenience: BiquadNode cutoff
ok = ex.set_param(gain, 0, 0.25)    # generic: (node, index, value) → False if queue full
assert ok
ex.process(np.ones((1, 512), np.float32))   # audio thread applies queued changes here
```

**C++** — the executor's live-param call is `postParam`:
```cpp
graph::GraphExecutor exec; exec.compile(g, 1, SR, 512);
exec.postParam(gain, graph::GainNode::kGain, 0.5f);      // == set_gain
exec.postParam(lp, graph::BiquadNode::kCutoffHz, 800.0f);// == set_cutoff
exec.postParam(gain, /*index*/ 0, 0.25f);                // generic
```

---

## 2. Automation from the control thread

To move a parameter over time — a fade, an LFO-swept filter, an envelope — step it from the
control thread and let the node's **smoothing** interpolate within each block. Update at
**control rate** (per UI event / every few ms / per block), **not per sample**: the command queue
is bounded and fixed at construction, so a flood returns `False` (the change is dropped, not
blocked) — that's the RT-safe backpressure. Coalesce to the latest value.

**Python** — a filter LFO alongside a running stream:
```python
import math, time, aiudio as a
# … build src → lp(biquad) → snk, compile ex …
# In production this loop runs while a backend drives process(); here we simulate with sleeps.
t0 = 0.0
for _ in range(200):
    hz = 500.0 + 450.0 * math.sin(2 * math.pi * 0.5 * t0)   # 0.5 Hz sweep, 50–950 Hz
    ex.set_cutoff(lp, hz)                                   # coalesced; smoothing avoids zipper
    t0 += 0.02
    time.sleep(0.02)                                        # ~50 Hz control rate
```

**C++**
```cpp
double t = 0.0;
for (int i = 0; i < 200; ++i) {
    const float hz = 500.0f + 450.0f * std::sin(2.0f * float(M_PI) * 0.5f * float(t));
    exec.postParam(lp, graph::BiquadNode::kCutoffHz, hz);
    t += 0.02;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}
```

> **Why not per-sample automation?** The command queue applies one value per block, and params
> smooth *within* the block, so per-sample posting would just overflow the queue. For
> sample-accurate modulation you'd use a modulation *node* (an `Oscillator` feeding a param is a
> Phase-1 feature); today, control-rate automation + smoothing covers faders, sweeps, and
> envelopes.

---

## 3. A control surface

The agent / a UI / an automation lane all reduce to the same thing: a **map from a name to a
`(node, param_index)`**, driven by `set_param`. Build the graph, remember the handles, expose
them.

**Python** — a tiny "mixer + tone" surface:
```python
import aiudio as a
SR = 48000.0
g = a.Graph()
i0, i1 = g.add_source(0), g.add_source(1)
mixer = g.add_mixer(2, 1.0)
tone  = g.add_biquad_lowpass(6000.0, 0.707, SR)
out   = g.add_gain(1.0)
snk   = g.add_sink(0)
g.connect(i0, 0, mixer, 0); g.connect(i1, 0, mixer, 1)
g.connect(mixer, 0, tone, 0); g.connect(tone, 0, out, 0); g.connect(out, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

controls = {                       # name → (node, param index)
    "ch1_level": (mixer, 0),
    "ch2_level": (mixer, 1),
    "tone_hz":   (tone, 0),
    "master":    (out, 0),
}
def set_control(name: str, value: float) -> bool:
    node, idx = controls[name]
    return ex.set_param(node, idx, value)   # RT-safe, click-free

set_control("ch2_level", 0.4)
set_control("tone_hz", 1200.0)
set_control("master", 0.8)
```

**C++** — the same idea with a `std::unordered_map<std::string, std::pair<NodeId,uint32_t>>`:
```cpp
std::unordered_map<std::string, std::pair<graph::NodeId, std::uint32_t>> controls = {
    {"ch1_level", {mixer, 0}}, {"ch2_level", {mixer, 1}},
    {"tone_hz",   {tone, 0}},  {"master",    {out, 0}},
};
auto setControl = [&](const std::string& n, float v) {
    auto [node, idx] = controls.at(n);
    return exec.postParam(node, idx, v);
};
setControl("tone_hz", 1200.0f);
```

---

## 4. Insert a node into a live chain

Adding processing while running: **edit the graph, then `compile()`** — the swap is atomic, so
the audio thread jumps from the old chain to the new one at a block boundary with no glitch (aside
from the effect itself engaging). Adding + rewiring frees nothing, so it's safe to edit **in
place** and recompile.

Example — insert a compressor between `gain` and `sink` on a live `src → gain → sink`:

**Python**
```python
# live graph: src → gain → snk  (already compiled + running)
comp = g.add_compressor(-18.0, 3.0, 5.0, 90.0)   # 1) add the node
g.disconnect(gain, 0, snk, 0)                    # 2) unhook the old edge
g.connect(gain, 0, comp, 0)                      # 3) rewire through the new node
g.connect(comp, 0, snk, 0)
ok, err = g.validate(); assert ok, err           # 4) validate BEFORE swapping
ex.compile(g, channels=1, sample_rate=SR, max_block=512)   # 5) atomic swap → compressor is live
```

**C++**
```cpp
const auto comp = g.addNode(std::make_unique<graph::CompressorNode>(-18.0f, 3.0f, 5.0f, 90.0f));
g.disconnect(gain, 0, snk, 0);
g.connect(gain, 0, comp, 0);
g.connect(comp, 0, snk, 0);
if (!g.validate().ok) return;                    // diagnose before swapping
exec.compile(g, 1, SR, 512);                     // atomic swap
```

> **Always `validate()` before `compile()`.** `compile()` rejects an invalid graph (cycle, a
> multiply-driven input port, a dangling edge) and returns `false`/keeps the *old* schedule live —
> but validating first gives you the human-readable reason instead of a silent no-swap.

---

## 5. Bypass / rewire live

Bypassing an effect is just a rewire + recompile: route around it. Because `disconnect` only
removes edges (it frees nothing), the currently-running schedule is unaffected until the swap.

**Python** — bypass `fx` in `src → fx → snk`:
```python
g.disconnect(src, 0, fx, 0)      # detach the effect's input
g.disconnect(fx, 0, snk, 0)      # detach its output
g.connect(src, 0, snk, 0)        # straight-wire around it
assert g.validate()[0]
ex.compile(g, channels=1, sample_rate=SR, max_block=512)   # bypassed at the next block
# …to re-engage: disconnect src→snk, reconnect src→fx→snk, recompile.
```

**C++**
```cpp
g.disconnect(src, 0, fx, 0); g.disconnect(fx, 0, snk, 0);
g.connect(src, 0, snk, 0);
exec.compile(g, 1, SR, 512);
```

> A tombstoned/bypassed node's *state* (filter history, delay line) is retained if you leave the
> node in the graph and only re-route edges — handy for click-free bypass. If you fully remove
> it (§6), that state is gone.

---

## 6. Remove a node safely while running

`remove_node` **frees the node instance immediately** (its slot is tombstoned so other ids don't
shift). The catch: a *currently-live* compiled schedule caches raw node pointers, so freeing a
node it still references would dangle until the swap+reclaim. Two safe options:

- **Preferred (RCU):** build the next topology as a **new `Graph`** (fresh instances) and
  `compile()` it. The atomic swap retires the old schedule; you keep the **old graph alive** until
  it's reclaimed (a few blocks). In **Python this is automatic** — `compile()` holds a reference
  to every graph it compiled (`keep_alive`), so the old instances outlive the old schedule.
- **Simplest:** `stop()` the backend, remove + recompile, `start()` again.

**Python** (RCU-style — rebuild without the node, swap):
```python
def rebuild_without(node_to_drop):
    ng = a.Graph()                               # fresh graph, fresh instances
    s = ng.add_source(); k = ng.add_sink()       # (rebuild the chain you still want)
    ng.connect(s, 0, k, 0)
    assert ng.validate()[0]
    ex.compile(ng, channels=1, sample_rate=SR, max_block=512)  # atomic swap; old graph kept alive
    return ng                                    # hold the new one; the executor keeps the old

g = rebuild_without(node_to_drop)                # the dropped node's schedule retires safely
```

For a *bounded, in-place* edit where you never free a live node, `remove_node` + `compile()` on
the same graph is fine **only after** you've ensured no live schedule references it — when in
doubt, prefer the rebuild above. (See [Appendix B](#appendix-b--lifetime--the-atomic-swap).)

**C++** — hold every compiled graph until retired (as the G5 test does):
```cpp
std::vector<std::unique_ptr<graph::Graph>> kept;   // keep graphs alive across swaps
kept.push_back(buildEditedTopology());             // fresh instances, without the dropped node
exec.compile(*kept.back(), 1, SR, 512);            // atomic swap; old schedule reclaimed off-thread
```

---

## 7. Read the graph back

A control surface / agent / serializer needs to *see* the current graph. `g.nodes()` returns the
live nodes as `(id, type_name, num_inputs, num_outputs)` (tombstoned ids are skipped);
`g.live_node_count` / `g.node_count` and `g.validate()` round it out.

**Python**
```python
for node_id, type_name, n_in, n_out in g.nodes():
    print(f"#{node_id:<3} {type_name:<16} {n_in} in / {n_out} out")
print("live nodes:", g.live_node_count, "| slots:", g.node_count)
print("valid:", g.validate())          # (ok, error)
```

**C++**
```cpp
for (std::uint32_t id = 0; id < g.nodeCount(); ++id)
    if (const graph::Node* n = g.node(id))
        std::printf("#%u %s  %u/%u\n", id, n->typeName(), n->numInputs(), n->numOutputs());
const auto v = g.validate();            // v.ok, v.error
```

---

## 8. Close the loop

Control without feedback is open-loop. Read telemetry to drive decisions: `ex.render_count`
(is the audio thread actually running / how many blocks?), a `Meter` node's level
(`g.meter_mean_square(node)`), and the xrun/telemetry counters from `docs/81` (source ring fill,
drift ratio, device/executor xruns). A closed control loop = *read a meter → set a param*.

**Python** — a crude auto-gain that reads the level and trims toward a target:
```python
import math
target_dbfs = -18.0
for _ in range(50):
    ex.process(block)                                  # (or: a backend is running it live)
    ms = g.meter_mean_square(meter)
    dbfs = 10 * math.log10(max(ms, 1e-12))
    if ex.render_count > 0 and dbfs > -120:
        adjust = 10 ** ((target_dbfs - dbfs) / 20.0)   # linear gain to hit the target
        ex.set_gain(trim, max(0.0, min(4.0, current_gain := adjust)))
```

**C++**
```cpp
const float ms = meter->meanSquare();
const float dbfs = 10.0f * std::log10(std::max(ms, 1e-12f));
if (exec.renderCount() > 0 && dbfs > -120.0f)
    exec.postParam(trim, graph::GainNode::kGain, std::pow(10.0f, (-18.0f - dbfs) / 20.0f));
```

---

## Appendix A — What's safe from the control thread

- ✅ **`set_param` / `postParam` / `set_gain` / `set_cutoff` / `set_q`** — always safe while
  running (lock-free queue, applied at the next block, click-free). Returns `False`/`false` if the
  queue is momentarily full — coalesce and retry.
- ✅ **`compile()`** — safe while `process()` runs; it builds off-thread and swaps atomically
  (RCU). Allocates, so it's a control-thread call, never an audio-thread one.
- ✅ **Reading telemetry/introspection** — `render_count`, `meter_mean_square`, ring/drift
  counters, `nodes()`, `live_node_count`, `validate()` — all safe to read any time.
- ⚠️ **Editing the `Graph` structure** (`connect`/`disconnect`/`add_*`/`remove_node`) — do it on
  the **control thread only**, then `compile()` to publish. The audio thread never sees the graph,
  only compiled schedules. `remove_node` frees an instance — see §6 / Appendix B.
- ⛔ **Never** call `process()` from two threads, mutate a compiled schedule, or free a node a live
  schedule still references.

## Appendix B — Lifetime & the atomic swap

`compile()` builds a `CompiledGraph` (its own buffers + **cached raw `Node*` pointers**) and
installs it with `active_.exchange(...)` — one atomic pointer swap. The audio thread loads
`active_` each block, so it runs exactly one schedule per block; the **retired** schedule is
deleted only after the audio thread has advanced past it (RCU-style reclamation), so there's no
free-under-the-reader race *for the schedule*.

The **nodes**, though, are owned by the `Graph`, not the schedule. So the invariant is:
**a `Graph` (and its node instances) must outlive every compiled schedule that still references
it.** Consequences:

- **Python:** `compile()` keeps a reference to each graph it compiled, so you can't accidentally
  free a graph whose schedule is still live. Rebuilding-and-recompiling (§6) is therefore safe by
  construction.
- **C++:** *you* own the `Graph`. Keep it alive until its schedule is retired; the idiomatic
  pattern (the G5 `test_graph_live_edit`) is to push each graph into a `std::vector` that outlives
  the executor. Adding/rewiring in place is safe (nothing frees); `removeNode` frees immediately,
  so prefer building a new graph for removals.

## Appendix C — Cross-references

- **Per-function API + the control setters:** [`docs/80`](80-pipeline-capabilities.md) §6–§7.
- **Node param indices:** [`docs/82`](82-node-usage-patterns.md#appendix--parameter-index-quick-reference).
- **Where these graphs run (clocks/backends):** [`docs/81`](81-pipeline-usage-patterns.md).
- **Why (decisions):** ADR-0002 (C++ core + Python control), ADR-0004 (audio thread sacred),
  ADR-0005 (compiled schedule / swappable clock), ADR-0009 (graph IR), ADR-0010 (control plane).
- **Milestones:** G5 (atomic schedule swap), G7 (control command queue) — `docs/74`.
- **Runnable examples:** `examples/python/ex_live_control.py`, `examples/cpp/ex_graph_live_edit.cpp`.

# 50 — Architecture & Design Patterns

Design patterns for the aiudio engine: the signal-graph IR, evaluation models,
the Python↔C++ boundary, differentiable rendering, and real-time-safe threading.

> **Provenance key:** **✓ Verified** = cited from the deep-research pass.
> **○ Background** = established engineering knowledge / synthesis, confirm before
> relying. Much of this doc is *synthesis* — the verified pass gave threading and
> differentiable-graph anchors, but graph-engine and interop design choices are
> aiudio's to make.

---

## 1. The central design problem

> **One graph IR must serve four masters at once:** (a) **real-time** block
> processing (<10–20 ms, no allocations), (b) **offline** high-quality rendering,
> (c) **differentiable** end-to-end training, and (d) an **LLM agent** that edits
> it. These pull in different directions; reconciling them is the core
> architectural work.

A useful framing: separate **what the graph *is*** (a typed, serializable IR) from
**how it *runs*** (multiple execution backends over the same IR).

```
            ┌──────────────────────────────────────────────┐
            │  Graph IR  (typed nodes + edges + params)      │  ← agent edits this (40-*)
            └──────────────────────────────────────────────┘
              │              │                │
   ┌──────────▼───┐  ┌───────▼────────┐  ┌────▼─────────────┐
   │ RT executor  │  │ Offline exec.  │  │ Diff. executor   │
   │ (C++, block, │  │ (C++/Py, big   │  │ (autograd, train │
   │  no-alloc)   │  │  non-causal)   │  │  through graph)  │
   └──────────────┘  └────────────────┘  └──────────────────┘
```

This "one IR, many backends" shape is what lets the *same* "brighten the vocal"
graph be trained offline, then run real-time.

---

## 2. Signal-graph engine: the design axes ○

Drawing on the prior art in `10-*` (Faust algebra, Web Audio AudioNode graph,
SuperCollider UGen graph, Max/Pd patchers):

| Axis | Options | Lean for aiudio | Why |
|---|---|---|---|
| **Evaluation** | Eager vs lazy | **Lazy/compiled** for the IR, eager for prototyping | A compiled schedule enables RT-safety + optimization (Faust/SC pattern) |
| **Direction** | Push vs pull | **Pull** (demand-driven) at block granularity | Web Audio / SC model; natural for variable sink demand |
| **Topology** | Static vs dynamic | **Static-by-default, edit-then-recompile** | RT threads can't reshape graphs mid-block; agent edits trigger recompiles off-thread |
| **Granularity** | Sample vs block | **Block** (with sample-accurate params) | Required for neural nodes + SIMD; matches plugin hosts |
| **Cycles** | DAG vs feedback | DAG + explicit **unit-delay** for feedback | Feedback needs a 1-sample delay node (classic DSP) |
| **Scheduling** | Topological static schedule | **Yes**, computed at compile | Deterministic RT execution; ANIRA-style pooling for heavy nodes |

**Node contract (the unifying abstraction, ✓-grounded in DDSP + Neutone):**
every node — classic DSP or neural — exposes:
1. `process(block) -> block` — the forward audio render (Neutone-style clean
   block contract ✓).
2. **differentiable parameters** + optional `backward` — so the graph trains
   end-to-end (DDSP contract ✓, `20-*`).
3. metadata: **`realtime_capable: bool`**, latency, causal?, sample-rate
   constraints — so the scheduler/agent can reason about it (✓-motivated by the
   RT/offline split).

---

## 3. Differentiable rendering of the whole graph ✓

The framework's pillar-3 superpower: **the graph itself is differentiable**, so
you can optimize structure/params against an audio objective.

- **Precedent (✓):** the **differentiable music-mixing-graph** work
  ([arXiv:2406.01049](https://arxiv.org/pdf/2406.01049)) shows a **DAG of audio
  processors can be reverse-engineered from input/output audio pairs** via a
  *fully differentiable* implementation of both the **processors and the
  pruning** — i.e. **gradient-based graph search** replaces discrete
  combinatorial search. This is a direct, verified proof-of-concept for a
  *differentiable graph engine*, not just differentiable nodes.
- **Connection to the agent (✓ synthesis, `40-*`):** Text2FX optimizes *effect
  params* against a CLAP embedding by gradient descent. Generalize that to the
  whole aiudio graph and you get: *agent proposes structure → gradient descent
  tunes the entire graph to match the perceptual objective.*
- **Hard constraints (✓, `20-*`):** differentiability is **not free** —
  uninformative oscillator-frequency gradients (F2) and recursive/IIR filters
  resisting autodiff (§`20-*` 2.2) mean some nodes need custom backward passes or
  surrogate gradients. The IR should let a node **declare** its differentiability
  status (fully / surrogate / non-diff).

---

## 4. Real-time-safe threading ✓

The verified anchors (`30-*`) give a concrete, validated threading design:

1. **The audio thread is sacred** (✓): no allocation, no locks, no syscalls. At
   48 kHz / 128 samples ≈ **2.7 ms** per block.
2. **Two execution classes for nodes** (✓):
   - **Inline RT-safe nodes** — small models / classic DSP, run *on* the audio
     thread, all memory pre-allocated (**RTNeural** pattern). ✓
   - **Off-thread pooled nodes** — heavy neural models, inference dispatched to a
     **static, pre-allocated thread pool** and results returned a block later
     (**ANIRA** pattern). ✓
3. **Lock-free hand-off** (○ best-practice): SPSC ring buffers / atomics between
   the audio thread and worker pool; double-buffering for parameter updates.
4. **Latency compensation** (✓ via Neutone's "delay compensation"): off-thread
   nodes introduce reported latency the host/graph must compensate.
5. **Graph edits happen off the audio thread** (○): the agent/UI mutates a
   *staging* copy of the IR; a compiled schedule is swapped in atomically at a
   block boundary (RCU-style), so the RT thread never reshapes a live graph.

> **✓ Architectural lesson reused:** ANIRA proves *one RT-safe layer can wrap
> multiple ML runtimes (LibTorch/ONNX/TFLite)*. aiudio's node executor should do
> the same — runtime-agnostic neural nodes behind the node contract.

---

## 5. The Python ↔ C++ boundary ○ (background — not in verified pass)

> The locked decision (`00-*`) is **C++ real-time core + Python research/ML
> layer**. The verified pass did **not** cover interop tooling; this is
> background to confirm.

**The split:**
- **C++:** the RT-safe engine, graph executor, scheduler, plugin hosting, the
  inline-node fast path. No Python on the audio thread — **ever** (GIL +
  allocation = RT death).
- **Python:** model authoring/training, the agent orchestration, dataset tooling,
  high-level graph construction & scripting, offline rendering.

**Interop tooling (○):**
- **nanobind** (Wenzel Jakob) — successor to pybind11; **lower overhead, smaller
  binaries, faster compile** — the modern default for new C++↔Python bindings.
- **pybind11** — mature, ubiquitous, huge ecosystem; the safe conservative
  choice.
- **TorchScript / `torch.export` / LibTorch** (✓-adjacent) — the proven model
  hand-off path (nn~/RAVE/ANIRA all cross the boundary as serialized Torch
  models, then run in C++).

**The key rule (✓-motivated):** Python builds and trains the graph; the graph is
**serialized to a language-neutral IR + Torch/ONNX model artifacts**; the C++
core loads and runs it **without** calling back into Python on the audio path.
Python may drive *control-rate* changes (agent edits, automation) via a lock-free
queue, never sample-rate work.

---

## 6. Putting it together — a reference architecture (synthesis ○/✓)

```
┌─────────────────────────── Python (research / control) ────────────────────────────┐
│  Agent (LLM)  ──edits──►  Graph IR builder   ──trains──►  Differentiable executor    │
│   (40-*)                   (typed nodes)                  (autograd; 20-*, §3)        │
│        │                        │  serialize (IR + Torch/ONNX artifacts)             │
└────────┼────────────────────────┼───────────────────────────────────────────────────┘
         │ control-rate edits      │  (nanobind / LibTorch boundary, §5)
         ▼ (lock-free queue)       ▼
┌─────────────────────────── C++ (real-time core) ────────────────────────────────────┐
│  Compiled schedule  ─►  RT executor (block, no-alloc)                                 │
│                          ├─ inline nodes  (RTNeural pattern, ✓)                       │
│                          └─ pooled nodes  (ANIRA static thread pool, ✓)               │
│  Host integration: VST3 / AU / CLAP / standalone (JUCE) ── Neutone/nn~ interop (✓)    │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 7. Open architecture questions (carried to `60-*`)

1. **Eager vs lazy / IR format** — exact IR representation and how aggressively to
   compile (Faust-style) vs interpret. *No verified guidance; aiudio's call.*
2. **nanobind vs pybind11** and how much of the offline path stays in Python vs
   moves to C++. *Background only — needs its own pass + benchmarks.*
3. **How to make the *whole* graph differentiable** robustly given F2 + IIR
   issues — which nodes get exact vs surrogate gradients. *✓ hard problems named.*
4. **Off-thread latency budget** — how big a model can run "real-time" once the
   ANIRA pooling latency is included. *✓ pattern known; numbers need measuring.*

See `60-gaps-and-opportunities.md` for prioritization.

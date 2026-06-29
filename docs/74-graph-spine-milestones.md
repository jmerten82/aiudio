# 74 — Graph Spine Milestone Plan

The **graph spine** is the backbone that turns the I/O backends (M0–M5) into an
actual processing **pipeline**: a typed graph IR + a node contract + an eager
executor. It is the substrate every later capability — DSP/neural nodes, the
differentiable executor, the agent — attaches to. Decisions: **ADR-0009**.

> Relationship to the I/O plan (`docs/71`): the I/O layer is the *edges*
> (sources/sinks); the spine is the *middle*. They meet at `SourceNode`/`SinkNode`
> and at the Python bindings (this plan's G6 = I/O M8). The spine does **not**
> require the remaining I/O milestones (M6/M7/M9) — it pulls them in when useful.
> G7 (ADR-0010) extends the spine to the **control plane**: Python drives a *running*
> pipeline (lock-free command queue + atomic telemetry) and the RT backend is exposed
> as a control-only frontend — the hook the agent (Phase 2) will use.

---

## 1. The one idea

> **The graph executor *is* a `RenderCallback`.** You build a typed graph (nodes +
> edges), the executor **compiles it once into a static topological schedule** with
> pre-allocated edge buffers, and its `process(in, out, frames, time)` runs that
> schedule eagerly per block. Because the executor is a `RenderCallback`, **any
> backend we already built (M2 output, M3 input, M4 duplex, M5 tap) drives a whole
> graph** — the spine plugs straight into the I/O foundation.

```
   M4 duplex backend ──drives──►  GraphExecutor : RenderCallback
                                    │  (compiled static schedule)
                                    ▼
       SourceNode ─► GainNode ─► MeterNode ─► SinkNode      (planar float32 blocks)
```

Audio format on every edge is the engine lingua franca from `docs/73`: **planar
float32, block-based, ±1.0**.

---

## 2. Core abstractions (the contracts to get right first)

```cpp
// A graph-level processor: N input ports + M output ports (each a planar buffer).
class Node {
public:
    virtual ~Node() = default;
    virtual void prepare(double sampleRate, std::uint32_t maxBlock) = 0;  // setup; may allocate
    virtual void process(const AudioBuffer* inputs, AudioBuffer* outputs,
                         std::uint32_t numFrames, const TimeInfo& time) noexcept = 0;  // RT
    virtual std::uint32_t numInputs()  const noexcept = 0;
    virtual std::uint32_t numOutputs() const noexcept = 0;
    // + parameters + metadata (channels/port, latency, realtimeCapable) — minimal at first
};

// The typed IR: nodes + directed edges (srcNode:outPort -> dstNode:inPort).
struct Edge { NodeId src; uint32_t srcPort; NodeId dst; uint32_t dstPort; };
class Graph { /* addNode(unique_ptr<Node>) -> NodeId; connect(Edge); validate() */ };

// Compiles a Graph to a static schedule (topo order + pre-allocated edge buffers)
// and runs it. IS a RenderCallback, so any AudioBackend can drive it.
class GraphExecutor : public RenderCallback {
public:
    bool compile(const Graph&, double sampleRate, std::uint32_t maxBlock);  // off-thread
    void process(const AudioBuffer& in, AudioBuffer& out,
                 std::uint32_t frames, const TimeInfo&) noexcept override;  // RT
};
```

`RenderCallback` (M1) is unchanged; `Node` is the new graph abstraction.
`SourceNode`/`SinkNode` bridge the executor's `in`/`out` to graph edges.

---

## 3. Milestones

Estimates are rough order-of-magnitude for one developer.

> **Progress:** G1 ✅ merged — the `aiudio-graph` library: the `Node` contract,
> the typed `Graph` IR with `validate()` (port-range, single-driver, acyclicity),
> and trivial `GainNode` / `SumNode`.
> G2 ✅ merged — the **`GraphExecutor`**: `compile()` builds a static topological
> schedule with pre-allocated per-port buffers (fan-out / fan-in); `process()`
> runs it as a `RenderCallback` (RT-safe). `SourceNode` / `SinkNode` bridge the
> executor's `in`/`out`. Verified: CTest, ASan/UBSan-clean, offline render → 1.300.
> G3 ✅ merged ⭐ — **first live end-to-end**: a compiled graph driven by the M4
> duplex backend (the executor IS a `RenderCallback`, so no glue). Added
> `MeterNode` (passthrough + level). Verified on macOS 26: the silent
> `ex_graph_capture_probe` ran `Source → Meter → Gain(0) → Sink` **live through the
> aggregate-device backend** (566 blocks executed by the IOProc → PASS) **and** the
> audible `ex_graph_live_passthrough` (mic → graph → speakers) confirmed on
> hardware. CTest 7/7. **The Phase-0 first-end-to-end deliverable is done.**
> G4 ✅ merged (shared with I/O M6) — **node library + offline backend**:
> `BiquadNode` (RBJ low-/high-pass) + the M6 `OfflineBackend` + WAV I/O; a
> `file → graph → file` render is a deterministic golden test (bit-exact).
> G5 ✅ merged — **live graph edits**: `compile()` installs a new schedule with an
> **atomic swap** (RCU); the audio thread picks it up next block and the old
> schedule is reclaimed off-thread (renderCount handshake). TSan-clean
> edit-while-running stress test. The hook the agent uses to edit a running graph.
> G6 ✅ merged (PR #12, == I/O M8) — **Python bindings** (nanobind): the `_aiudio`
> module exposes `Graph` (node factories, `connect`, `validate`, `set_gain`,
> meter), `GraphExecutor` (`compile` + a **numpy `process()`** bridge), and
> `OfflineBackend`. Verified on macOS 26: `_aiudio` builds clean; from Python a
> `source → gain → sink` graph runs and returns numpy (out == in·gain); a live
> `set_gain` takes effect; `pip install .` (scikit-build-core) yields an importable
> `aiudio` package. **The graph spine (G1–G6) is complete.**
> G7 ✅ merged (PR #13) — **Python control plane** (ADR-0010): a
> lock-free SPSC **command queue** in `GraphExecutor` (`postParam`, drained at the
> top of each block) + `Node::setParam`; live, RT-safe `set_gain` / `set_cutoff` /
> `set_q` / `set_param` from Python, plus `render_count` telemetry. The **output
> `DeviceBackend`** (Core Audio) is bound as a **control-only frontend** — Python
> opens/starts/stops + controls + observes; the audio thread stays pure C++ and the
> GIL is released on lifecycle calls. Verified on macOS 26: C++ CTest 12/12
> (incl. the new `test_graph_control`); the command plane is **TSan-clean** under a
> flood-while-rendering stress test;
> `ex_live_control.py` ran a live output stream (render_count climbing at ~94
> blocks/s) driven entirely from Python without touching the audio thread.

### G1 — Node & Graph IR types (+ trivial nodes)
- **Deliverable:** `aiudio-graph` library: `Node`, `Graph` (add/connect/validate),
  `Edge`, port/buffer types; two trivial nodes — `GainNode`, `SumNode` (mixer).
  Unit tests for graph construction + validation (cycle/port-mismatch rejection).
- **Acceptance:** build a 3-node graph in code; `validate()` accepts a DAG and
  rejects a cycle and a port mismatch; `GainNode`/`SumNode` unit-tested in isolation.
- **Est.** 3–4 d.

### G2 — Compiler + eager executor (offline-tested)
- **Deliverable:** `GraphExecutor::compile()` (topological schedule + per-edge
  buffer allocation) and `process()`; runs a graph block-by-block. Golden-file
  **offline** tests: feed a known input array through `Source → Gain → Sink`,
  assert the output (bit-exact for gain).
- **Acceptance:** offline render of a multi-node graph matches expected output;
  `process()` is allocation-free (verified under a no-alloc check / ASan); executor
  is a working `RenderCallback`.
- **Est.** 4–6 d.

### G3 — Source/Sink + first live end-to-end ⭐
- **Deliverable:** `SourceNode`/`SinkNode` adapting the executor's `in`/`out`;
  drive `capture → GainNode → MeterNode → playback` **live** via the M4 duplex
  backend. A silent objective probe (callbacks + level) + the hands-on audible run.
- **Acceptance:** the Phase-0 "first end-to-end" line — capture → trivial graph →
  playback runs live, glitch-free, on real hardware.
- **Est.** 3–4 d.

### G4 — A small node library + offline backend
- **Deliverable:** a handful of nodes (mixer/`SumNode`, `MeterNode`, a biquad EQ,
  passthrough) + pull in **I/O M6** (file/offline backend) so a `file → graph →
  file` render is a golden-file test.
- **Acceptance:** render a wav → graph → wav offline, deterministic; the same graph
  runs live (G3) and offline unchanged.
- **Est.** 4–6 d.

### G5 — Edit + recompile + atomic swap
- **Deliverable:** off-thread graph edits → recompile → **atomic schedule swap** at
  a block boundary (RCU-style, ADR-0005); the audio thread never sees a half-edited
  graph. This is the mechanism the agent will drive.
- **Acceptance:** add/remove/reconnect a node while audio is running; no glitch, no
  data race (TSan-clean); old schedule freed safely.
- **Est.** 4–5 d.

### G6 — Python bindings (= I/O M8)
- **Deliverable:** nanobind bindings to build/edit graphs and add `Source`/`Sink`
  from Python; numpy block access; drive the executor from a backend.
- **Acceptance:** from Python, construct `source → gain → sink`, run it live, and
  read/measure output blocks as numpy.
- **Est.** 3–4 d.

### G7 — Python control plane + RT backend frontend (ADR-0010)
- **Deliverable:** a lock-free **command queue** (`GraphExecutor::postParam`, drained
  at the top of `process()`) + uniform `Node::setParam`; live RT-safe parameter control
  from Python (`set_gain`/`set_cutoff`/`set_q`/`set_param`); **atomic telemetry**
  (`render_count`, meter); and the **output `DeviceBackend`** bound as a *control-only
  frontend* (GIL released on `open`/`start`/`stop`). The audio thread never runs Python.
- **Acceptance:** from Python, start a live output stream, change parameters while it
  runs, and read telemetry — all without touching the audio thread; the command plane is
  TSan-clean under a flood-while-rendering stress test.
- **Out of scope (follow-ups):** binding the input/duplex/tap backends; an oscillator
  node for audible output; parameter automation curves.
- **Est.** 3–4 d.

---

## 4. Dependency graph

```
G1 (IR + node contract) ─▶ G2 (compiler + executor) ─▶ G3 (source/sink, live) ─┬─▶ G4 (node lib + offline)
                                                                               ├─▶ G5 (edit/recompile/swap)
                                                                               └─▶ G6 (Python = I/O M8) ─▶ G7 (control plane + RT frontend)
```
G1 gates everything (the contract). G2 before G3 (need an executor before wiring
to devices). G4/G5/G6 are parallelizable after G3. G7 builds on G6 (Python) + G5
(atomic swap) — it adds the live control queue + the device-backend frontend.

---

## 5. Definition of done — "the spine is built"

1. A typed graph **compiles to a static schedule** and runs **RT-safe** (no
   allocation/locks on the audio thread).
2. The **same graph** runs **live** (G3, via any backend) and **offline**
   (G4, golden-file) through the *same* IR.
3. Graphs are **editable off-thread** with an atomic swap (G5) — no glitches/races.
4. Graphs are **buildable and drivable from Python** (G6), and a **running** pipeline
   is **controllable + observable from Python** as a frontend, RT-safely (G7).
5. The node contract has survived a real multi-node consumer — ready for the
   **differentiable executor** (Phase 1) and the **agent** (Phase 2) to attach (the
   agent drives G7's command queue + G5's atomic swap).

---

## 6. What the spine deliberately does NOT do (yet)

- **No differentiability** — Phase 1 adds a second executor over the *same* IR.
- **No agent** — Phase 2 edits the IR (G5's swap is the hook).
- **No feedback cycles** (need unit-delay nodes), **no multi-rate/control-rate**,
  **no off-thread neural-node pooling** (ANIRA pattern, ADR-0006) — later.
- **No serialization format** yet — designed-for, deferred.

## 7. Risks

| Risk | Mitigation |
|---|---|
| Node contract wrong for composition (multi-input, ports) | G1+G3 are the cheap validators; evolve the contract *now*, before more nodes |
| Buffer planning / aliasing in the schedule | start simple (one buffer per edge); optimize (in-place/reuse) only if measured |
| RT-safe atomic graph swap is subtle | TSan in G5; RCU/double-buffer the compiled schedule (mirrors the M1 ring-buffer discipline) |
| Scope creep toward the full engine | this plan is the *spine* only; §6 is the hard boundary |

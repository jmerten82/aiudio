# ADR-0010: Python control plane — lock-free command queue, atomic telemetry, RT backend as a control-only frontend

- **Status:** Accepted
- **Date:** 2026-06-29
- **Deciders:** Project owner
- **Related:** ADR-0002 (C++ RT core + Python research/control layer), ADR-0004 (the
  audio thread is sacred), ADR-0005 (one duplex callback, swappable clock), ADR-0009
  (graph spine — recompile + atomic swap). Plan: `docs/pipeline/74-graph-spine-milestones.md`
  (G7). Builds on G6's nanobind bindings.

## Context

G6 gave Python the ability to **build, compile, and run** graphs (numpy + the offline
backend). The natural next ask is to let Python **drive a *running* pipeline** — start a
real device, tweak parameters, read levels — i.e. act as the control surface / frontend
that the future agent (Phase 2) and any UI will need.

This collides head-on with the central invariant (ADR-0004): the audio thread must never
allocate, lock, block, or run Python (the GIL). G6's `Graph.set_gain` mutated a node field
directly from the control thread — safe only when stopped, a data race on a live stream.
So "control a running pipeline from Python" needs a deliberate, RT-safe boundary, not a
direct setter. We also need a way to *observe* a running engine (is audio flowing? what
level?) without polling across an unsafe boundary.

The decision: define **how** Python controls and observes the live engine, and **how much**
of the RT backend Python may touch.

## Decision

**1. Control is data, not code — a lock-free SPSC command queue.** Control-rate parameter
changes are posted as trivially-copyable `ParamCommand{node, param, value}` records onto a
pre-allocated `io::RingBuffer` owned by the `GraphExecutor`. The control thread (Python /
the agent) is the single producer (`GraphExecutor::postParam`); the audio thread is the
single consumer, **draining the queue at the top of each `process()` block** before
rendering. Wait-free, allocation-free, bounded — it never blocks or races the audio thread
(ADR-0004 §2). `postParam` returns `false` if the queue is momentarily full (the caller may
retry); it never grows or allocates.

**2. Nodes apply changes via a uniform `Node::setParam(index, value) noexcept`.** Default
no-op; each node defines its own indices (`GainNode::kGain`, `BiquadNode::kCutoffHz`/`kQ`).
`setParam` runs **on the audio thread** during the drain, so it must be RT-safe (recomputing
biquad coefficients with `sin`/`cos` is fine; no allocation/locks). Tweakable scalars that
both threads see (e.g. `GainNode`'s gain) are `std::atomic`, read once into a local in the
hot loop so vectorization is preserved.

**3. Telemetry is published by the audio thread via atomics; Python polls.**
`GraphExecutor::renderCount()` (blocks processed) and `MeterNode::meanSquare()` (level) are
relaxed/acquire atomics the audio thread writes and the control thread reads. No callback
from the audio thread into Python, ever.

**4. The RT device backend is exposed to Python as a *control-only frontend*.** Python may
`enumerate`/`open`/`start`/`stop` a Core Audio backend (`DeviceBackend`) and read its status;
the device IOProc calls the C++ executor directly. **Python is never invoked on the audio
thread.** Lifecycle calls (`open`/`start`/`stop`) **release the GIL** (`nb::gil_scoped_release`)
so the control thread never blocks the engine and `Ctrl-C` still works. There is **no** API
to pass a Python callable as the audio process function — that is explicitly forbidden.

**5. Scope of this step (G7).** Bind the **output** backend (`CoreAudioBackend`) as
`DeviceBackend` (no special permissions, no sound-input risk). Input / full-duplex / process-tap
backends (M3–M5) are bindable the same way later but are out of scope here. macOS-only symbols
(`DeviceBackend`, `AudioDeviceInfo`) are conditionally exported.

## Consequences

**Positive**
- Python (and later the agent) can drive a **live** pipeline — the control-plane pillar of the
  vision becomes real — while the audio thread stays pure C++. The boundary is *structural*:
  Python is incapable of stalling or racing the audio thread.
- One uniform mechanism for all live parameter edits (`set_param` + node `kSomething` indices),
  and one for structural edits (recompile + atomic swap, ADR-0009 §7). The agent will use
  exactly these two hooks.
- Telemetry (`render_count`, meter) gives a frontend a truthful, lock-free view of the engine.

**Negative / costs**
- A second way to set parameters (the queue) coexists with direct IR mutation + recompile;
  docs must steer live control to the queue. (`GainNode`'s gain is atomic, so the legacy direct
  path is at least not a data race.)
- The command queue is fixed-capacity: a flood drops commands (returns `false`) rather than
  blocking — correct for RT, but callers must tolerate it.
- The `GraphExecutor`'s queue is cache-line over-aligned (false-sharing avoidance), so it is
  held behind a `unique_ptr` to keep the executor's own alignment normal (nanobind cannot hold
  an over-aligned instance).

**Neutral / follow-ups**
- Time-varying **automation / modulation** can layer on top of the same queue.
- An **oscillator / file-source** node would make live device output audible (today an output
  device hands the graph an empty input → silence; the frontend still demonstrably runs).
- Binding the **input / duplex / tap** backends is a straightforward repeat of this pattern.

## Alternatives considered

- **Direct setters from the control thread (G6's `Graph.set_gain`)** — rejected for live use:
  a data race on the audio thread (ADR-0004). Kept only for the stopped/offline case.
- **A Python `process` callback driven by the audio thread** — rejected outright: acquiring the
  GIL on the audio thread can block unboundedly; violates the sacred-thread invariant.
- **A mutex around node parameters** — rejected: locks on the audio thread are forbidden
  (ADR-0004). The SPSC queue is the sanctioned cross-thread mechanism (ADR-0004 §2).
- **Per-parameter `std::atomic` only (no queue)** — viable for scalars but doesn't generalize to
  structural/compound edits or ordered batches; the queue subsumes it (and we still use atomics
  where a single scalar suffices).

## References
- ADR-0002/0004/0005/0009; `docs/pipeline/74-graph-spine-milestones.md` (G7);
  `include/aiudio/io/ring_buffer.hpp` (the SPSC mechanism), `bindings/aiudio_bindings.cpp`
  (`DeviceBackend`, `set_*`, telemetry), `examples/python/ex_live_control.py`,
  `notebooks/aiudio_pipeline_tour.ipynb` §7–§8.

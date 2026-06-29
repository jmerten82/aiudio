# ADR-0009: Graph spine — IR, node model, and eager executor

- **Status:** Accepted
- **Date:** 2026-06-28
- **Deciders:** Project owner
- **Related:** ADR-0003 (hybrid graph / one IR / node contract — this *concretizes*
  it), ADR-0004 (RT safety), ADR-0005 (duplex callback / swappable clock),
  ADR-0008 (multi-input). Plan: `docs/74-graph-spine-milestones.md`.

## Context

M0–M5 built the I/O layer — the *edges* of the system. Each backend (output,
input, full-duplex, tap) drives exactly **one** `RenderCallback`. To realize the
project thesis (classic DSP + neural models as first-class peers, differentiable,
agent-editable), we need the *middle*: the **graph engine**. ADR-0003 set the
high-level direction ("one typed IR, many backends, a universal node contract");
this ADR makes the concrete decisions for the **spine** — the minimal,
real-time-capable graph engine that turns the backends into a pipeline — while
deferring differentiability (Phase 1) and the agent (Phase 2).

The spine is on the critical path: the differentiable executor and the agent both
operate over the IR, so the IR must exist first. It is also the first real
*consumer* that composes multiple nodes, which is what validates whether the node
contract is right.

## Decision

**1. Node model.** Introduce a `Node` abstraction (the graph-level processor):
`prepare(sampleRate, maxBlock)` for setup/allocation, and
`process(const AudioBuffer* inputs, AudioBuffer* outputs, uint32 frames, TimeInfo)
noexcept` for the RT render over **N input ports + M output ports** (each a planar
float32 `AudioBuffer`), plus parameters and metadata (port counts/channels,
latency, `realtime_capable`). **`RenderCallback` (M1) is kept unchanged** as the
I/O-boundary contract; a 1-in/1-out node is its degenerate case.

**2. The graph executor IS a `RenderCallback`.** Its `process(in, out, …)` runs the
node graph internally. Therefore **any existing backend (M2–M5) drives an entire
graph**, not just one node — no new backend work is needed for the spine.

**3. Graph IR.** A typed structure: nodes (type + params) + directed edges
(`srcNode:outPort → dstNode:inPort`). In-memory C++ for the spine; a **serialized
form** (for persistence + the agent) is designed-for but **deferred**.

**4. Execution model: compile once to a static topological schedule, run eagerly
per block.** Single sample rate, block-based, planar float32 (per `docs/73`).
Topology is **static between recompiles** — the audio thread never mutates the
graph. **DAG only**; feedback cycles (which need explicit unit-delay nodes) are
deferred.

**5. Buffers.** The executor pre-allocates one buffer per edge at compile/prepare;
`process()` is **allocation-free and RT-safe** (ADR-0004). Edge channel counts are
fixed at compile time.

**6. Source/Sink bridge.** `SourceNode` / `SinkNode` adapt the executor's `in` /
`out` `AudioBuffer`s (from a capture / duplex / tap backend) to graph edges, so
the graph sits between a backend's input and output.

**7. Editing via recompile + atomic swap.** Graph edits happen **off the audio
thread**; a recompile produces a new schedule that is **atomically swapped** in at
a block boundary (RCU-style), per ADR-0005. This is the mechanism the agent will
later use.

**8. New module.** A new library target **`aiudio-graph`** depending on
`aiudio-io`.

**9. Explicitly deferred** (not in the spine): differentiable execution (Phase 1
adds a *second executor over the same IR*); the agent control plane (Phase 2);
multi-rate / control-rate signals; feedback cycles; off-thread pooling of heavy
neural nodes (ANIRA pattern, ADR-0006); the graph serialization format. The node
contract is *designed* to carry differentiable parameters later (ADR-0003), but
the spine executor is eager/RT only.

## Consequences

**Positive**
- The first real **pipeline**: capture → nodes → playback, live.
- Composing multiple nodes **validates the node contract** before more is built on it.
- Any backend drives a whole graph (reuse of M2–M5 for free).
- The IR is the **substrate** the differentiable executor and the agent attach to.

**Negative / costs**
- A new abstraction layer + buffer-planning/compiler complexity.
- Recompile-on-edit (acceptable; edits are not RT).
- The node contract may need to **evolve** as its first real consumer — expected,
  and a reason to build the spine *before* more backends.

**Neutral / follow-ups**
- `docs/74` sequences the build (G1–G6); G6 is the Python-bindings step
  (= I/O M8). Differentiable executor is a Phase-1 ADR over the same IR.

## Alternatives considered

- **Extend `RenderCallback` directly into a graph node** — rejected: conflates the
  I/O-boundary contract with the graph node; keep them separate (executor *is* a
  `RenderCallback`, nodes are a distinct type).
- **Lazy / pull / demand-driven per-sample evaluation** — rejected for the spine: a
  static DAG with a compiled topological schedule is simpler and RT-friendly.
  Revisit if dynamic/multi-rate needs arise.
- **Dynamic graph mutation on the audio thread** — rejected: violates ADR-0004;
  use recompile + atomic swap.
- **Build differentiability into the spine now** — rejected: premature; defer to a
  Phase-1 executor over the same IR (ADR-0003 keeps the contract ready for it).

## References
- ADR-0003/0004/0005/0008; `docs/50-architecture-patterns.md` §1–§3 (graph-engine
  design axes), `docs/73-digital-audio-encoding.md` (buffer format),
  `docs/74-graph-spine-milestones.md` (the plan).

# ADR-0014: Multi-source manager — N sources + M sinks on one clock via per-stream rings

- **Status:** Accepted
- **Date:** 2026-06-29
- **Deciders:** Project owner
- **Related:** **makes ADR-0008 §5 concrete** (the deferred "multi-source manager that owns N
  backends + N ring buffers"); builds on ADR-0009/ADR-0011 (graph spine + multi-stream
  executor, G10), ADR-0004 (RT safety). Plan: `docs/76` (multi-source I/O) **Phase C / M10**.

## Context

ADR-0008 decided the *model* for combining multiple audio sources — one backend per clock,
one SPSC ring per source, mixing as a graph node — and explicitly **deferred** the component
that coordinates them ("a future multi-source manager that owns N backends + N ring
buffers", §5). The spine prerequisites are now in `main`: a multi-stream executor that takes
N inputs / M outputs (G10/ADR-0011), per-port channel widths (G8), latency/delay-compensation
(G9), and the xrun policy + ring counters (M9.1). M10 builds the manager that ties them
together into a composable multi-source pipeline.

## Decision

**1. A `MultiSourceManager` owns the transport between N input sources, the graph, and M
output sinks.** It holds, per stream, one lock-free SPSC `RingBuffer` **per channel**
(ADR-0008 §2 — never merge producers into one ring) and the planar buffers the multi-stream
executor reads/writes.

**2. Three roles, three thread contexts (the ADR-0008 picture):**
- **Producers** call `pushInput(stream, planar, frames)` from each source's thread (a device
  capture IOProc, or Python). One producer per input stream.
- The **pump** calls `process(executor, frames, time)` on the **master clock** (a device
  IOProc) or a manual pump: it drains each input ring → executor inputs, runs the multi-stream
  graph (G10), and pushes the outputs → each output ring. It is the single consumer of every
  input ring and the single producer of every output ring.
- **Consumers** call `popOutput(stream, planar, frames)` from each sink's thread. One consumer
  per output stream.
Per-ring SPSC is preserved throughout; the pump is RT-safe (wait-free ring ops, bounded
copies, the RT-safe executor — no allocation/locks, ADR-0004).

**3. Degradation reuses the M9.1 xrun policy.** An input ring that hasn't been fed yields
**silence** (underrun, counted); an output ring whose sink isn't draining **drops** (overrun,
counted). `inputUnderruns(stream)` / `outputOverruns(stream)` expose the per-stream ring
counters.

**4. Mixing/routing stays in the graph** (ADR-0008 §4): the manager only moves frames between
rings and the executor's stream buffers; a `SumNode` + per-stream gains do the combining.

**5. Single-clock scope.** M10 composes streams that are driven by **one** clock (one master
pump). Bringing genuinely **off-clock** device sources onto the master timeline (adaptive
resampling / drift compensation) is **M9.5**, and binding a real device's IOProc as the master
pump (feeding/draining the rings) is the remaining live-integration layer. The manager's
design is agnostic to who pumps it.

**6. Python.** `MultiSourceManager(num_inputs, num_outputs, channels, max_block, ring_frames)`
with numpy `push_input`/`pop_output`, a `process(executor, frames)` pump (GIL released), and
the telemetry getters. Non-copyable (owns rings) — declared explicitly so nanobind doesn't try
to generate a copy (the `vector<unique_ptr<>>` "copyable-per-trait-but-ill-formed" trap).

## Consequences

**Positive**
- The capstone: **N independent sources → one graph (mix/route) → M sinks**, composable and
  RT-safe. The headline goal of `docs/76` is reachable.
- Reuses everything already merged (multi-stream executor, channel widths, PDC, xrun rings) —
  the manager is mostly *wiring*, which is why it stays small and testable.
- Deterministic + headless-testable (Python/threads play the source/sink roles); the live
  device master-clock is a thin layer on top.

**Negative / costs**
- One ring per (stream, channel) — more rings, but each stays a cheap wait-free SPSC and
  channels move in lockstep.
- True multi-*device* live capture still needs drift compensation (M9.5) for off-clock sources;
  M10 alone composes on a single clock.

**Neutral / follow-ups**
- Live wiring: a `RenderCallback` adapter that writes a device's `in` into an input ring (and
  reads an output ring into a device's `out`), with a duplex/master backend driving `process()`.
- The device↔graph channel-mapping slice of M9.2 folds in here naturally.

## Alternatives considered

- **One ring per source carrying interleaved frames.** Rejected: variable block sizes break
  frame-alignment in a flat interleaved ring; per-channel mono rings stay aligned for any block
  size and avoid an interleave/de-interleave step on the audio thread.
- **Merge sources before the boundary (one ring, many producers).** Rejected by ADR-0008 §2 —
  the SPSC ring is single-producer; merging is the graph's job after the boundary.
- **Bake multi-source into the executor.** Rejected: keeps clocking/transport (manager) separate
  from processing (graph) — they evolve independently (ADR-0008 §4).

## References
- ADR-0008 (§2/§4/§5), ADR-0011 (multi-stream executor), ADR-0009, ADR-0004; `docs/76`
  (Phase C / M10); `include/aiudio/graph/multi_source_manager.hpp`,
  `src/graph/multi_source_manager.cpp`, `bindings/aiudio_bindings.cpp`;
  tests `tests/test_multi_source_manager.cpp`, `testing/python/test_multisource.py`;
  example `examples/python/ex_multisource.py`.

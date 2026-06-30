# ADR-0013: Latency reporting + graph-wide delay compensation (PDC)

- **Status:** Accepted
- **Date:** 2026-06-29
- **Deciders:** Project owner
- **Related:** **extends ADR-0009** (graph spine — node contract, executor); ADR-0004 (RT
  safety). Plan: `docs/76` (multi-source I/O) **Phase A / G9**. Reused by **M9.3** (boundary
  sample-rate conversion) and future lookahead/FFT/neural nodes.

## Context

Real processing nodes delay the signal: a lookahead limiter, an FFT/overlap-add block, a
sample-rate converter, and most neural nodes all emit their output some frames after the
corresponding input. When such a node sits on **one branch of a fan-out** and another branch
is direct, summing the branches misaligns them → comb filtering / phase smear. The graph
spine (ADR-0009) had no notion of latency: the `Node` contract didn't report it and the
executor didn't compensate. G9 adds both — it's a prerequisite for M9.3 (a resampled source
must stay aligned with other sources) and for true multi-source mixing.

## Decision

**1. Nodes report processing latency — `Node::latencyFrames() const noexcept` (default 0).**
A node returns the number of frames it delays its output. Zero-latency nodes (gain, sum,
meter, biquad, source, sink, down/up-mix) inherit the default unchanged. An *intentional*
delay effect reports **0** (it must not be compensated away); only *inherent* processing
latency is reported.

**2. The executor runs a latency-propagation + delay-compensation pass at compile.** In
topological order `build()` computes the accumulated latency at each node's output
(`inMax + latencyFrames()`, where `inMax` is the max latency over its inputs). At any fan-in,
input ports with **less** accumulated latency than `inMax` are routed through a **compensating
`DelayLine`** of `inMax − portLatency` frames, so all of a node's inputs are time-aligned and
recombine in phase. `GraphExecutor::latencyFrames()` reports the graph total (max latency at a
sink).

**3. Compile-time wiring; `process()` stays RT-safe.** The compensation `DelayLine`s and their
delayed buffers are allocated at compile and stored in the (atomically-swapped) `CompiledGraph`.
On the audio thread, `process()` just runs each compensation line (a bounded per-channel ring
copy) before the node reads its inputs — **no allocation, no locks** (ADR-0004). Zero-latency
graphs allocate no delay lines and run exactly as before (the bit-exact golden render is
unchanged).

**4. A reusable `DelayLine` + a `LatencyNode`.** `DelayLine` is a fixed integer-sample,
per-channel delay (used by both the executor's compensation and by nodes). `LatencyNode(frames)`
delays by `frames` **and** reports that latency — it models any latency-introducing node and is
the test fixture that proves compensation. Python: `add_latency(frames)`, `latency_frames`.

**5. Scope fences.** Integer-frame latency only (sub-sample/fractional delay is deferred — it
arrives with the resampler, M9.3). Latency does not yet size per-node state (the deferred
`prepare()`-channel-count change, ADR-0012 §5); `LatencyNode` uses a constructor `maxChannels`
like `BiquadNode`. Feedback cycles remain disallowed (DAG only, ADR-0009).

## Consequences

**Positive**
- Parallel paths of differing latency **recombine in phase** automatically (PDC) — the
  building block multi-source mixing and the resampler (M9.3) require.
- A truthful **graph latency** is reported for downstream delay budgeting.
- **Zero cost / zero change** for graphs without latency nodes (no delay lines allocated;
  `process()` identical; golden render still bit-exact).

**Negative / costs**
- `build()` gains a topological latency pass + an edge scan per input port (off-thread setup).
- Per compensated input port: one delayed buffer + one `DelayLine` (memory) and a per-block
  ring copy (bounded CPU on the audio thread).
- `DelayLine` uses a per-sample modulo for the ring index — correct and bounded; a power-of-two
  / branchless wrap is a possible later optimization.

**Neutral / follow-ups**
- Fractional-delay compensation and reporting backend (device) latency layer on this.
- The resampler (M9.3) will report its latency through exactly this contract.

## Alternatives considered

- **Report latency only; leave compensation to the caller.** Rejected: the acceptance and the
  resampler need *automatic* in-phase recombination; manual alignment doesn't scale across a graph.
- **Insert explicit delay *nodes* into the graph during compile.** Rejected: it would mutate the
  user's IR and complicate the edit/recompile model; compensation as internal executor delay
  lines keeps the IR clean (mirrors how mixing is a node but compensation is plumbing).
- **Per-edge ring buffers exposed in the IR.** Over-engineered for integer-frame PDC; the
  executor-owned `DelayLine` is simpler and RT-safe.

## References
- ADR-0009; `docs/76` (Phase A / G9), `docs/74` (spine);
  `include/aiudio/graph/{node,delay_line,latency_node}.hpp`, `src/graph/graph_executor.cpp`
  (propagation + compensation), `bindings/aiudio_bindings.cpp`; tests
  `tests/test_graph_latency.cpp`, `testing/python/test_latency.py`.

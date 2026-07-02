# ADR-0011: Multi-stream executor — N input/output streams via source/sink binding

- **Status:** Accepted
- **Date:** 2026-06-29
- **Deciders:** Project owner
- **Related:** **extends ADR-0009** (graph spine — single in/out executor); ADR-0008
  (multi-input model — per-source rings + a future manager), ADR-0004 (RT safety),
  ADR-0005 (one duplex callback). Plan: `docs/pipeline/76` (multi-source I/O) **Phase D / G10**.

## Context

ADR-0009 built the executor around a **single** input and a **single** output buffer:
`RenderCallback::process(const AudioBuffer& in, AudioBuffer& out, …)`, and the executor
fed **every** `SourceNode` the same `in` and **every** `SinkNode` the same `out`. That is
correct for one capture stream → graph → one playback stream, but it cannot express **true
multi-source I/O** (N independent input sources, M output sinks in one graph) — the goal of
`docs/pipeline/76`. ADR-0008 already decided the *transport* model (one backend per clock, one SPSC
ring per source, mixing as a graph node) and deferred the manager that coordinates them.
The piece missing on the **graph side** is an executor that can *receive* N inputs and
*drive* M outputs and let nodes bind to a specific one. This is G10, the first PR of the
multi-source chain, and the foundation the manager (M10) will feed.

## Decision

**1. The executor gains a multi-stream entry point.** Add
`process(const AudioBuffer* inputs, uint32 numInputs, AudioBuffer* outputs, uint32
numOutputs, uint32 numFrames, TimeInfo)`. It routes `inputs[k]` to Source nodes bound to
stream `k` and `outputs[k]` to Sink nodes bound to stream `k`.

**2. Source/Sink nodes carry a stream index.** `SourceNode(stream=0)` / `SinkNode(stream=0)`
gain an immutable `streamIndex()`. Default `0` preserves all existing behavior.

**3. The single `in`/`out` `process()` is the 1-stream special case.** The existing
`RenderCallback` override is retained and simply delegates to the multi-stream form with
`numInputs == numOutputs == 1`. **Existing backends, tests, examples, and the notebook are
unchanged** (a default graph uses only stream 0). Back-compatibility is a hard requirement.

**4. Out-of-range streams degrade safely.** A Source whose `streamIndex ≥ numInputs` emits
silence (its external input is null); a Sink whose `streamIndex ≥ numOutputs` writes nothing.
No error, no crash — consistent with the graceful-degradation discipline.

**5. Compile-time only; the hot path is unchanged in cost.** Stream routing is an index
compare per Source/Sink; `process()` stays allocation-free and lock-free (ADR-0004). The
executor exposes `channels()`, `inputStreamCount()`, `outputStreamCount()` (= max bound
stream index + 1) for callers/bindings to size the buffers they pass.

**6. Python.** `Graph.add_source(stream=0)` / `add_sink(stream=0)`; `GraphExecutor.
process_multi(list_of_arrays, num_outputs=0) -> list_of_arrays` (numpy per stream);
`channels` / `input_streams` / `output_streams` properties. The single-array `process()` is
unchanged.

**7. Channels stay uniform for now.** Each stream's buffer uses the one compiled channel
count; per-port/per-stream channel *widths* are a separate change (G8, ADR to come). G10 is
about the *number of streams*, not their channel widths.

## Consequences

**Positive**
- The graph can now **receive N inputs and drive M outputs**; mixing/routing remain ordinary
  nodes (`SumNode`, future routing nodes) — ADR-0008 §4 honored.
- It is **fully offline-testable with no hardware** (feed N numpy arrays, read M back), which
  is why it is the first, lowest-risk PR of the multi-source chain.
- Zero behavior change for single-stream graphs; the 1-stream path is literally a one-line
  delegate, so the whole existing suite stays green.

**Negative / costs**
- A second `process` overload to keep in mind; mitigated by the single form delegating.
- Streams are still uniform-channel-width (G8 needed for per-stream widths).

**Neutral / follow-ups**
- The **multi-source manager (M10)** will drive this entry point from N device-backed SPSC
  rings on one master clock; off-clock sources are aligned by M9 (resample/drift). G10 is the
  graph-side half; M10 is the I/O-side half (`docs/pipeline/76` Phases C/D).

## Alternatives considered

- **Concatenate all sources into one wide buffer, select channel ranges.** Rejected as the
  primary path: it conflates *sources* with *channels*, forces G8 first, and doesn't map
  cleanly to distinct clocks/rates (which want distinct streams, ADR-0008 §2). It remains a
  valid fast path for the "all sources share a clock and channel layout" case.
- **A new multi-IO callback type instead of an executor method.** Rejected: the
  `RenderCallback` is the *backend↔engine* boundary (one duplex callback, ADR-0005); the
  multi-stream entry is an *executor* capability the manager drives, not a device contract.
- **Make every node stream-aware.** Unnecessary: only the boundary nodes (`Source`/`Sink`)
  bind to streams; interior nodes are stream-agnostic.

## References
- ADR-0008/0009; `docs/pipeline/76` (Phase D / G10), `docs/pipeline/74` (spine);
  `include/aiudio/graph/{source_node,sink_node,graph_executor}.hpp`,
  `src/graph/graph_executor.cpp`, `bindings/aiudio_bindings.cpp`;
  tests `tests/test_graph_multistream.cpp`, `testing/python/test_multistream.py`.

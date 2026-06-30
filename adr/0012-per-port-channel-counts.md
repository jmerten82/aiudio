# ADR-0012: Per-port channel counts — channel-width propagation in the executor

- **Status:** Accepted
- **Date:** 2026-06-29
- **Deciders:** Project owner
- **Related:** **extends ADR-0009** (graph spine — uniform channel count fixed at compile);
  ADR-0004 (RT safety). Plan: `docs/76` (multi-source I/O) **Phase A / G8**.

## Context

ADR-0009 fixed a **single, uniform channel count** for the whole compiled graph: the
executor allocated every port buffer at the one `numChannels` passed to `compile()`. That
makes channel-count-*changing* operations — down-mix (stereo→mono), up-mix (mono→stereo),
split/merge, mixdown — impossible to express, because no port can be a different width than
any other. It is also a prerequisite for placing several sources side-by-side as channels in
true multi-source I/O (`docs/76`). G8 lifts the uniform-width restriction while keeping the
real-time path untouched.

The change is cheap by construction (noted in the feasibility analysis): `AudioBuffer`
already stores its own `numChannels`, and the executor already allocates **one buffer per
output port** and walks the graph in **topological order** — so per-port widths ride
existing structure. The only genuinely new piece is *computing* each port's width.

## Decision

**1. Nodes declare their output widths from their input widths — `Node::channelLayout()`.**
`channelLayout(const uint32* inWidths, uint32 numIn, uint32* outWidths, uint32 numOut,
uint32 hostChannels)`. The **default** is *inherit / broadcast*: 0-input nodes emit
`hostChannels`; all others emit `max(inWidths)` on every output. This default is correct for
**every existing channel-agnostic node** (gain, sum, meter, biquad, source, sink) — none
needs an override. Only width-*changing* nodes override it.

**2. The executor runs a channel-width propagation pass at compile.** `build()` now: topo
order → propagate widths in that order (each node's `channelLayout` from its resolved input
widths; unconnected inputs default to `hostChannels`) → **allocate each output-port buffer at
its own computed width** (the shared zero buffer at the widest width seen) → build the
schedule. **Compile-time only; `process()` is unchanged and stays allocation-/lock-free**
(ADR-0004). Generic nodes already adapt to buffer width at runtime (`c < numChannels` loops),
so they need no change.

**3. The first width-changing nodes ship with it.** `DownmixNode` (N→1, channel average) and
`UpmixNode` (1→N, duplicate) override `channelLayout`. Python: `add_downmix()` /
`add_upmix(channels=2)`.

**4. The I/O boundary stays at the host width (scope fence).** Source/Sink nodes keep the
compiled host channel count; per-*stream* widths (different widths per input stream) are a
later concern. G8 makes *interior* widths flexible. `GraphExecutor::channels()` still reports
the host width.

**5. `prepare()` is unchanged.** The demonstrator nodes are stateless. A stateful node that
must size per-channel state to its *actual* width (e.g. a biquad fed a non-host width) would
need its channel count at `prepare()` — deferred until a node needs it, to avoid a
contract-wide signature ripple now. Existing nodes are unaffected (biquad keeps its
constructor `maxChannels`, passthrough beyond it — pre-existing behavior).

## Consequences

**Positive**
- Channel-count-changing routing (down/up-mix, and the split/merge/mixdown family) is now
  expressible — a building block for mixers and for multi-source composition (`docs/76`).
- **Zero behavior change for uniform-width graphs**: every port resolves to the host width,
  reproducing the old uniform allocation exactly (the bit-exact golden render still passes).
- No RT-path cost: widths are baked into the compiled schedule; `process()` is untouched and
  still allocation-free (the RT-safety allocation test stays green).

**Negative / costs**
- `build()` does an extra topological propagation pass and an edge scan per input port
  (off-thread/setup; same complexity class as the existing schedule build).
- Stateful nodes can't yet size state to a non-host width (deferred `prepare()` change, §5).

**Neutral / follow-ups**
- A general **routing / mix-matrix** node (same-width, no engine change) and split/merge
  (multiple output ports of different widths — already enabled here) are natural next nodes.
- Per-*stream* channel widths at the I/O boundary pair with the multi-source manager (M10).

## Alternatives considered

- **Fixed per-node port widths + strict edge validation** (no inference). Rejected: it would
  force channel-agnostic nodes (gain, biquad) to be instantiated per channel count; inference
  keeps them width-generic.
- **Keep uniform width; fake channel changes by zero-padding.** Rejected: wastes buffers and
  can't represent a genuinely narrower port (down-mix would still carry N channels).
- **Change `prepare()` now to pass channel counts.** Deferred: a contract-wide ripple with no
  current consumer (the demonstrators are stateless); add it when a stateful node needs it.

## References
- ADR-0009; `docs/76` (Phase A / G8), `docs/74` (spine);
  `include/aiudio/graph/{node,downmix_node,upmix_node}.hpp`, `src/graph/graph_executor.cpp`
  (the propagation pass), `bindings/aiudio_bindings.cpp`; tests
  `tests/test_graph_channels.cpp`, `testing/python/test_channels.py`.

# ADR-0008: Multi-input & full-duplex clocking — shared clock, aggregate devices, per-source ring buffers

- **Status:** Accepted
- **Date:** 2026-06-28
- **Deciders:** Project owner
- **Related:** ADR-0004, ADR-0005, ADR-0007; `docs/pipeline/71-io-layer-milestones.md` (M4, M5, M9); `docs/theory/50-architecture-patterns.md`

## Context

ADR-0005 established "one duplex callback, swappable clock." M2 (output) and M3
(input) each drive **one device on its own clock**. Two upcoming needs force a
clocking decision:

1. **Full-duplex (M4)** — live `capture → process → playback` needs input and
   output to advance on **one coherent clock**; otherwise round-trip timing is
   undefined.
2. **Multiple simultaneous inputs** — e.g. mic + a second interface + a process
   tap (M5) feeding one pipeline.

The hard fact (macOS/Core Audio, and audio generally): **separate physical
devices run on independent sample clocks that drift.** Pairing an independent
input IOProc with an independent output IOProc — or two independent input IOProcs
— yields streams that are not sample-aligned and slip over time. The M1
`RingBuffer` is **single-producer/single-consumer** (ADR-0004), so it is *not* a
place to merge multiple producers.

On the dev machine this is concrete: the mic (Sennheiser input) and speakers
(Kanto output) are *different* Core Audio devices, so naive full-duplex would
drift.

## Decision

**1. Full-duplex runs on a single shared clock.**
- If the chosen input and output are the **same** Core Audio device, run **one
  duplex IOProc** on it.
- If they are **different** devices, create a Core Audio **aggregate device**
  spanning them (output as the clock master, **drift compensation** enabled on
  the other sub-device) and run **one IOProc** on the aggregate.
- We will **not** synchronize full-duplex by pairing two independent IOProcs.

**2. Each off-clock source gets its own SPSC `RingBuffer`.**
A separate capture device, a process tap (M5), network audio, or a Python
consumer each crosses the thread boundary through its **own** single-producer/
single-consumer ring buffer. The consumer/graph reads N buffers. We never feed
one ring buffer from multiple producers.

**3. Sample-accurate alignment of multiple devices uses aggregate devices first,
resampling second.** Core Audio aggregate devices provide hardware/driver drift
compensation across sub-devices. Where devices cannot be aggregated, the source
is brought onto the engine clock by **software resampling / drift compensation**
(M9).

**4. Mixing/summing is a graph-node responsibility, not the I/O layer.** The I/O
layer's job ends at delivering each source's frames on a coherent clock (or via a
ring buffer). Combining multiple sources into one signal is done by a mixer node
once the graph engine exists.

**5. One backend instance owns one clock source** (one device or one aggregate
device). Coordinating several is the job of a future multi-source manager that
owns N backends + N ring buffers.

## Consequences

**Positive**
- Full-duplex is sample-accurate by construction; round-trip latency is
  well-defined and measurable.
- Multi-input has a clear, uniform model (one SPSC ring buffer per source) that
  preserves the cheap wait-free ring buffer.
- Separation of concerns: clocking/transport in the I/O layer, mixing in the
  graph.

**Negative / costs**
- Aggregate-device creation/teardown is fiddly Core Audio plumbing (CFDictionary
  config, sub-device UIDs, drift-comp flags) and is macOS-specific.
- Aggregating devices adds some latency vs. a single duplex device.
- Software resampling (fallback path) costs CPU and adds latency; deferred to M9.

**Neutral / follow-ups**
- M4 implements paths (1) (same-device duplex and aggregate-device duplex).
- A `MultiSourceManager` + mixer node are future work (graph spine, M5+).

## Alternatives considered

- **Independent input + output IOProcs for full-duplex** — rejected: they drift;
  round-trip timing undefined.
- **Always create an aggregate device** (even when one device does both) —
  rejected: needless setup + latency when a single device suffices.
- **One MPMC/MPSC ring buffer fed by multiple producers** — rejected: defeats the
  cheap SPSC design (ADR-0004), and merging belongs in the graph, not the buffer.

## References

- ADR-0004 (RT safety / SPSC ring buffer), ADR-0005 (duplex callback / swappable
  clock), ADR-0007 (Core Audio).
- `docs/pipeline/71-io-layer-milestones.md` §M4 (full-duplex), §M9 (drift/multi-device).
- Apple Core Audio aggregate devices (`AudioHardwareCreateAggregateDevice`,
  `kAudioAggregateDevice*`/`kAudioSubDevice*` keys).

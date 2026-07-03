# ADR-0015: Boundary sample-rate conversion + cross-clock drift compensation

- **Status:** Accepted
- **Date:** 2026-06-29
- **Deciders:** Project owner
- **Related:** **realizes ADR-0008** ("aggregate-then-resample"; one backend per clock) and
  builds on **ADR-0013** (latency reporting) + **ADR-0014** (multi-source manager). Bound by
  ADR-0004 (RT safety) and ADR-0005 (swappable clock). Plan: `docs/pipeline/76` (multi-source I/O)
  **Phase B — M9.3 / M9.5 / M9.6**.

## Context

A multi-source pipeline must combine audio that arrives at **different sample rates** (a
44.1 kHz file into a 48 kHz engine) and, worse, on **different physical clocks** (a USB mic
and the built-in output each run their own crystal). Even two nominally-"48 kHz" devices
drift by tens of PPM, so a producer feeding a consumer at the "same" rate will, over minutes,
slowly over- or under-fill the ring between them → an eventual xrun. ADR-0008 anticipated
this and fixed the strategy ("**aggregate-then-resample** onto the master timeline; one
backend per clock"), but deferred the mechanism. This ADR is that mechanism, built in three
steps: the converter (M9.3), the control loop that makes it adaptive (M9.5), and the
two-device composition that uses both (M9.6).

## Decision

**1. Resampling happens at the I/O *boundary*, never as a graph node.** The typed graph IR
is single-rate (ADR-0009): every node in a compiled graph runs at one engine sample rate. A
sample-rate change is therefore an **edge** concern — it sits between a device/source clock
and the engine, in the I/O layer — not a node. `io::Resampler` is that boundary unit. This
keeps the IR clean (mirrors ADR-0013's "compensation is plumbing, not a node").

**2. `io::Resampler` — a streaming, RT-safe, fixed-*or*-adaptive fractional SRC.** Planar
float32; `ratio = inputRate / outputRate` (input frames consumed per output frame). A 4-tap
**Catmull-Rom** (cubic Hermite) kernel over a per-channel window gives smooth, allocation-free
conversion that reproduces DC and linear ramps exactly. `process()` is *pull*-shaped —
produce up to `outCap` frames, consume up to `inAvail`, report both — so a caller pulling a
fixed engine block keeps leftover input across calls (seamless block boundaries). State (window
+ fractional phase) persists, so streaming == one-shot. It reports `latencyFrames()` (the
kernel group delay, in output frames) through the **ADR-0013** contract, so a resampled source
stays sample-aligned with the others.

**3. Drift is an *adaptive ratio*, driven by a ring-fill control loop (M9.5).** The same
`Resampler.setRatio()` is nudged every block by a **`DriftCompensator`**: it watches the fill
level of the SPSC ring between an off-clock source and the engine, compares it to a target
(half-full), and applies a small proportional correction to the resample ratio so the ring
stays centered. Slightly too much input → ratio rises (consume faster); too little → ratio
falls. Corrections are clamped (a few hundred PPM) so a transient can't destabilize pitch.
This is a classic asynchronous-sample-rate-converter (ASRC) servo; it needs no knowledge of
the true clock ratio, only the observable fill error.

**4. One backend per clock; the master clock ticks, off-clock devices are resampled (M9.6).**
Per ADR-0005/0008, exactly one backend is the master clock (its IOProc drives
`MultiSourceManager::process`). Every other device runs on its own clock and crosses to the
engine timeline through its own ring + `Resampler` + `DriftCompensator`. The first
cross-clock composition — **one input device + one output device on separate clocks** (M9.6) —
is the stepping stone to N sources (it is *not* itself N sources; that's Phase C).

**5. Everything stays off the audio thread's critical rules (ADR-0004).** `Resampler` and
`DriftCompensator` allocate only at `prepare()`; `process()`/`setRatio()`/the servo update are
wait-free (no locks, no allocation, no exceptions). Cross-thread transport remains the
lock-free SPSC ring (ADR-0008 §2).

## Consequences

**Positive**
- Sources at different rates **and** different clocks combine on one engine timeline without
  drift xruns — the precondition for true multi-device / multi-source live I/O.
- The converter is reusable: offline rate conversion, file→engine, and (later) neural-model
  rate adaptation all use the same `io::Resampler`.
- Adaptive ratio reuses the fixed converter unchanged — `setRatio()` was designed in from M9.3,
  so M9.5 adds only the servo, not a second resampler.

**Negative / costs**
- Cubic interpolation is good but not transparent at extreme ratios; a polyphase-FIR / sinc
  upgrade (or libsamplerate/Speex, ADR-0008 noted as ○ background) can replace the kernel later
  behind the same interface.
- The drift servo adds a tuning surface (gain, clamp, target fill); mis-tuning shows as slow
  pitch wobble. Tuned conservatively and covered by a long soak test.
- True hardware multi-device verification needs two physical devices on different clocks; the
  logic is proven headlessly by driving two `MockBackend`s at different effective rates.

**Neutral / follow-ups**
- Higher-order / polyphase kernel; reporting and composing device (backend) latency with the
  kernel latency; clock-drift *estimation* telemetry surfaced to Python.

## Alternatives considered

- **Resampling as a graph node.** Rejected: it would make the IR multi-rate and break the
  single-rate executor contract (ADR-0009). Rate change is a boundary concern.
- **Block-rate (integer) resampling only.** Rejected: cannot track sub-sample clock drift; the
  whole point of M9.5 is fractional, continuously-adjustable conversion.
- **A single master rate forced on all devices (no SRC).** Rejected: impossible across distinct
  hardware clocks (they drift regardless of nominal rate) and across mixed-rate files.
- **Timestamp-based resync (drop/insert samples) instead of a servo.** Rejected as the primary
  path: audible clicks; the ASRC servo is continuous and click-free. (Sample drop/insert remains
  a last-resort fallback if a ring still under/overflows.)

## References
- ADR-0008 (aggregate-then-resample, one-backend-per-clock), ADR-0013 (latency), ADR-0014
  (manager); `docs/pipeline/76` Phase B (M9.3/M9.5/M9.6);
  `include/aiudio/io/resampler.hpp`, `include/aiudio/graph/drift_compensator.hpp` (M9.5);
  `bindings/aiudio_bindings.cpp`; tests `tests/test_resampler.cpp`,
  `testing/python/test_resampler.py`.

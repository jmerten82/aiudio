# ADR-0005: I/O foundation — one duplex callback, swappable clock

- **Status:** Accepted
- **Date:** 2026-06-28
- **Deciders:** Project owner
- **Related:** `docs/pipeline/71-io-layer-milestones.md` §1–§2, `docs/pipeline/70-*`, ADR-0003, ADR-0004

## Context

aiudio must capture from devices, system output, and per-app audio, and play back
to devices — and also run inside a plugin host (where the host owns I/O) and
offline (file → file). These look like different problems, but in a real-time
engine the **output callback is the master clock** that drives the graph, and
input + output meet inside one render step. Modeling input and output as two
separate subsystems would fork the clock and the threading model.

## Decision

**We will expose a single real-time entry point — `process(in, out, frames,
time)` — driven by a swappable clock ("backend").** Backends: a **device** backend
(output IOProc is the clock, standalone), a **plugin-host** backend (the host's
`processBlock` is the clock), and an **offline** backend (a manual pump). Sources
that run on a *different* clock (process taps, network, Python) reconcile via
lock-free ring buffers. This mirrors JUCE / PortAudio / plugin-SDK conventions
deliberately.

## Consequences

**Positive**
- Input and output are one layer with one contract; the same graph runs under
  standalone, plugin, and offline clocks unchanged.
- Clean place to measure and report round-trip latency for compensation.

**Negative / costs**
- Multi-device capture/playback can drift; prefer a single duplex IOProc /
  aggregate device, add drift compensation later.
- The plugin-host backend must be designed early so the abstraction doesn't
  ossify around aiudio owning the device.

**Neutral / follow-ups**
- Defines the I/O milestone plan (`docs/pipeline/71-*` M0–M9) and the `AudioBackend` /
  `RenderCallback` interfaces.

## Alternatives considered

- **Separate input and output engines** — forks the clock/threading model; harder
  full-duplex latency control.
- **Device-ownership baked into the engine** — breaks plugin hosting, where the
  host owns I/O.

## References

- `docs/pipeline/71-io-layer-milestones.md`, `docs/pipeline/70-macos-audio-capture-plan.md`.

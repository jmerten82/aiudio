# ADR-0007: macOS audio I/O via Core Audio (HAL + process taps)

- **Status:** Accepted
- **Date:** 2026-06-28
- **Deciders:** Project owner
- **Related:** `docs/pipeline/70-macos-audio-capture-plan.md`, ADR-0005

## Context

The first development platform is macOS 26 (verified on the target machine,
`docs/pipeline/70-*`). aiudio needs input-device capture, **system-output** capture, and
**per-application** capture, plus device playback. macOS has no native loopback
input. Three options exist for output/per-app capture (verified `docs/pipeline/70-*`):
Core Audio **process taps** (macOS 14.4+), **ScreenCaptureKit**, and virtual
devices (**BlackHole**). Process taps are native, low-latency, pure-audio,
per-process, and integrate with the HAL aggregate-device model.

## Decision

**On macOS, we will build the I/O layer on Core Audio:** the **HAL**
(`AudioObject*` enumeration, `AudioDeviceCreateIOProcID`) for device input/output,
and **Core Audio process taps** (`CATapDescription` →
`AudioHardwareCreateProcessTap` → aggregate device) for system and per-app output
capture. **ScreenCaptureKit** is the alternative *only* when screen/video is also
needed; **BlackHole** is a zero-code prototyping fallback, not the shipping path.

## Consequences

**Positive**
- Native, lowest-latency, per-process capture with no kernel extension or
  user-installed virtual device; one HAL model for devices + taps.
- Fits the swappable-clock backend abstraction (ADR-0005) cleanly.

**Negative / costs**
- Taps require macOS 14.4+ and a **signed binary with
  `NSAudioCaptureUsageDescription`** + TCC approval — a real packaging constraint
  (`docs/pipeline/70-*` §6); bare CLI/Python fails silently.
- Core Audio is C-level and under-documented; budget for the tap setup ceremony.
- macOS-specific; other platforms need their own backend behind the same
  interface.

## Alternatives considered

- **ScreenCaptureKit for system audio** — heavier Screen-Recording permission,
  AV-oriented, self-signed permission issues; better only if capturing video too.
- **BlackHole/virtual device as the product path** — manual routing, no per-app
  granularity, extra user install; fine for experiments only.

## References

- `docs/pipeline/70-macos-audio-capture-plan.md`; Apple Core Audio taps docs &
  insidegui/AudioCap (`docs/90-references.md`).

# 75 — M9 Robustness & Hardening: Implementation Plan

> **Last updated:** 2026-06-29 · **Milestone:** I/O **M9** (`docs/71` §M9) — the last
> I/O milestone, **not a Phase-0 gate**. Status: **planned** (Phase-0 foundation is
> complete; this is productionization that overlaps Phase 3).
>
> Provenance per `CLAUDE.md` §8: **✓ Verified** = grounded in the current code; **○
> Background** = standard DSP/Core Audio knowledge to confirm before relying on it.

M9 is the milestone that makes the I/O layer survive the real world. Unlike M0–M8, it is
**a basket of largely independent sub-features**, not one deliverable — `docs/71` even
tags it *"ongoing."* This plan splits it into sub-milestones (**M9.1–M9.6**), each with
its own deliverable / acceptance / estimate / RT-safety note / dependencies, and
recommends a sequence so the cheap, high-value parts land first and the hard pro-audio
parts (drift, multi-device) are deferred until actually needed.

---

## 1. The one governing principle

Every M9 feature is a variation on one rule (ADR-0004 — *the audio thread is sacred*):

> **The audio thread degrades gracefully and never blocks. Detection, accounting, and
> recovery happen off-thread; the callback only ever substitutes safe data and moves on.**

Concretely: underruns emit **silence**, overruns **drop** (the SPSC ring already does this
— ✓ `ring_buffer.hpp` `push()`/`pop()` return `false` rather than block/overwrite),
counters are **atomics published as telemetry** (like `GraphExecutor::renderCount()`), and
anything that allocates or reconfigures (bigger buffers, re-open a device, resize state)
runs on the **control thread**, then publishes via the existing atomic-swap path.

A qualitative note that shapes effort: the per-port-channel-count work (channel routing)
is *compile-time only* and adds nothing to the hot path, but **SR conversion and drift
compensation add per-block DSP cost on the audio thread** — they must fit the xrun budget,
so they need profiling, not just correctness.

---

## 2. Scope — what M9 is, and what it is **not**

**In scope (M9.1–M9.6 below):** xrun/underrun policy, channel mapping/routing, boundary
sample-rate conversion, device hot-plug/disconnect, drift compensation, multi-device.

**Explicitly out of scope** (so M9 doesn't inflate):
- **In-graph multi-rate** — a node running at a different sample rate than the host (e.g. a
  16 kHz neural node inside a 48 kHz graph). That's an executor/spine change and belongs to
  **Phase 1** (neural). M9's SR scope is the **I/O boundary only** (device SR ≠ host SR).
- **Plugin-host backend** — that's **M7** (host-owns-I/O), a separate milestone.

---

## 3. Sub-milestones

Estimates are order-of-magnitude for one developer (same scale as `docs/71`). RT column:
🟢 off-thread / compile-time only · 🟡 light per-block cost · 🔴 real per-sample DSP on the
audio thread.

### M9.1 — xrun / underrun policy ⟶ *do first*
- **RT:** 🟡 · **Est:** 2–3 d · **Deps:** none · **ADR:** no (applies ADR-0004)
- **Why first:** it makes every later live/soak test *trustworthy* — you can only trust a
  "no glitches" run if glitches are actually detected and counted.
- **Deliverable:**
  1. **Detection.** Rings already signal it (✓ `push/pop → false`, `read/write → short
     count`). Add **device-side gap detection**: derive `TimeInfo.sampleTime` from the HAL
     timestamp instead of the current naïve `sampleTime_ += frames` (✓ Verified naïve today,
     `coreaudio_backend.cpp:114`) so a dropped block is visible; optionally subscribe to
     `kAudioDeviceProcessorOverload` (○ Background — confirm the property/listener API).
  2. **Substitution (RT-safe).** Define + wire the contract: **underrun → zero-fill** the
     output block (optionally a short fade); **overrun → drop** (no overwrite, no block).
  3. **Accounting.** `std::atomic<uint64_t>` xrun counters on the backend/executor, exposed
     as telemetry (`xrun_count`, mirroring `render_count`).
  4. **Tolerance knob** in `StreamConfig` (e.g. `best_effort` vs `strict`) for "count &
     continue" vs "stop on first xrun."
- **Acceptance:** a forced overload (heavy fake `process()`) increments `xrun_count` and
  produces silence, not garbage/crash; a slow ring consumer reports drops; `xrun_count == 0`
  over a clean 60 s run. Telemetry readable from Python.

### M9.2 — Channel mapping & routing
- **RT:** 🟢 (compile-time) · **Est:** 2–4 d **+ ~3–5 d for the per-port-channel-count
  prerequisite** · **Deps:** the per-port-channel-count engine feature · **ADR:** yes (extends
  ADR-0009's uniform-channel assumption)
- **Two layers:**
  - **Device↔graph mapping** (the boundary): map an N-channel device to the graph's channel
    count — select/duplicate/mix channels in `SourceNode`/`SinkNode` or the backend. Includes
    **mono↔stereo** (the acceptance case).
  - **Within-graph routing** (swap, split, merge, matrix, up/down-mix): needs **per-port
    channel counts** — every port can declare its own width, resolved by a channel-count
    *propagation pass* in `GraphExecutor::build()` (✓ the executor already allocates one
    buffer per output port and walks topologically, so this rides existing structure; ✓
    `AudioBuffer` already stores `numChannels`). Same-width routing (an N×N matrix) is a pure
    node needing **no engine change** and can ship immediately.
- **Acceptance:** a stereo device drives a mono graph and vice-versa, correctly; an N×N
  channel-matrix node; a downmix (2→1) and a split (one 2-ch port → two 1-ch ports) render
  correctly offline (golden). `process()` stays allocation-free (it's all compile-time).
- *(Full analysis of the per-port-channel-count feature: §"per-port channel count" — it is
  a contained, compile-time change plus a mechanical `prepare()`-signature ripple.)*

### M9.3 — Boundary sample-rate conversion
- **RT:** 🔴 · **Est:** 4–6 d · **Deps:** M9.1 (the resampler is a new xrun source) · **ADR:**
  yes (adds latency + per-block cost to the I/O path)
- **Deliverable:** a fixed-ratio **polyphase FIR resampler** (or integrate libsamplerate /
  Speex resampler — ○ Background, pick after a quality/latency/licence comparison) inserted at
  the I/O boundary when device SR ≠ host SR. Pre-allocated, allocation-free in `process()`,
  reports its **latency** (needs the node/edge latency field — see §5).
- **Acceptance:** a 44.1 kHz device feeds a 48 kHz graph (and back) with no audible artifacts
  and a measured, reported added latency; CPU within the xrun budget at 128 frames.

### M9.4 — Device hot-plug / disconnect + fallback
- **RT:** 🟢 (recovery is off-thread) · **Est:** 4–7 d · **Deps:** M9.1 · **ADR:** no
- **Deliverable:** subscribe to Core Audio HAL notifications (device list / default-device /
  device-died — ○ Background, confirm the `AudioObjectAddPropertyListener` set) on a listener
  thread; a **state machine** that, on disconnect mid-stream, stops the dead IOProc cleanly,
  falls back to the default device (or surfaces a callback), and re-opens — all off the audio
  thread, coordinating with `start()/stop()/open()`.
- **Acceptance:** unplugging the active USB interface mid-stream → **clean fallback, no crash,
  no hang**, an event surfaced to the control layer; re-plug recovers.
- **Risk:** hard to test automatically (needs a physical unplug or a mock HAL) — see §6.

### M9.5 — Drift compensation (separate clocks) ⟶ *defer*
- **RT:** 🔴 · **Est:** 1.5–2.5 wk · **Deps:** M9.3 (adaptive resampling), M9.6 · **ADR:** yes
- **Deliverable:** when input and output run on **different** physical clocks they drift; add
  an adaptive resampler driven by a control loop on ring fill-level (or insert/drop samples) to
  hold them in lock. A classic hard real-time problem.
- **Acceptance:** input and output on two different devices show **no drift / no creeping
  latency over 10 min** (the `docs/71` no-drift criterion, for the multi-device case).
- **Why defer:** M4's single duplex IOProc / aggregate device (ADR-0008) avoids drift for the
  common case; this is only needed for true multi-device. High risk, high cost.

### M9.6 — True multi-device ⟶ *defer*
- **RT:** 🔴 · **Est:** 1–1.5 wk (couples with M9.5) · **Deps:** M9.5 · **ADR:** yes
- **Deliverable:** drive input and output on **separate** physical devices (not an aggregate),
  with M9.5 holding them in sync.
- **Acceptance:** mic on device A → speakers on device B, glitch-free, no drift over 10 min.
- **Why defer:** aggregate devices (M4) cover most real needs; this + M9.5 is where weeks go.

---

## 4. Dependency graph & two delivery targets

```
M9.1 xrun policy ──┬─▶ M9.3 boundary SRC ──▶ M9.5 drift comp ──▶ M9.6 multi-device
                   ├─▶ M9.4 hot-plug/fallback
                   └─▶ M9.2 channel map/route   (needs: per-port channel counts)
```

- **Minimum-viable M9** — hits M9's *own* acceptance (survive unplug; resample 44.1↔48;
  mono↔stereo) = **M9.1 + M9.2 + M9.3 + M9.4 ≈ 2.5–4 weeks.**
- **Full M9 as written** — add **M9.5 + M9.6** (drift + multi-device) ≈ **6–8 weeks total**,
  with drift comp the dominant risk/time sink. Recommend stopping at minimum-viable until a
  concrete multi-device need appears.

---

## 5. Cross-cutting engine prerequisites this surfaces

- **Per-port channel counts** (for M9.2) — a contained, compile-time change to
  `GraphExecutor::build()` (a channel-count propagation pass) + a `Node::channelLayout` rule +
  a `prepare()`-signature ripple. **ADR-worthy** (extends ADR-0009).
- **Latency reporting on `Node`/backend** (for M9.3, and reused by lookahead/FFT/neural nodes)
  — add `latencyFrames()` + graph-wide delay compensation. ✓ Verified absent today (the `Node`
  contract has no latency field). **ADR-worthy.** Cheapest high-leverage change; do it with M9.3.

---

## 6. Testing approach (ties to `testing/README.md`)

M9 stresses the **liveness** layer hardest, and two items can't be unit-tested:
- **M9.1 / M9.3 / M9.2** — testable headless: forced-overload xrun counts; golden offline
  renders for channel maps and resampling (extend `test_cross_backend.py`); RT-safety via the
  **allocation test** (`testing/cpp/test_rt_safety_alloc.cpp`) extended to the resampler path.
- **M9.4 / M9.5 / M9.6** — need **hardware-in-the-loop or a mock Core Audio backend**:
  a fake backend that can inject "device died" notifications and clock drift, plus long-running
  **soak tests** (10-min drift, mid-stream unplug). Building that mock backend is a real,
  budgeted chunk of the M9.4–M9.6 cost and the main verification risk.

---

## 7. ADRs to write

| Sub-feature | ADR | Why |
|---|---|---|
| Per-port channel counts (M9.2) | extend/supersede ADR-0009 | changes the "uniform channel count fixed at compile" decision |
| Boundary resampling + latency reporting (M9.3, §5) | new | adds per-block cost + a latency-compensation model to the I/O path |
| Drift compensation / multi-device clocking (M9.5/6) | new (relates to ADR-0008) | a second clocking strategy beyond the shared-IOProc/aggregate model |

xrun policy (M9.1) and hot-plug (M9.4) are *applications* of ADR-0004/0005 — no new ADR; they
go in code review + `CLAUDE.md` conventions.

---

## 8. Definition of done (from `docs/71` §M9)

- [ ] Survives unplugging the USB interface mid-stream — clean fallback, no crash (M9.4).
- [ ] SR mismatch (44.1↔48) resampled transparently (M9.3).
- [ ] mono↔stereo mapping correct (M9.2).
- [ ] xrun/underrun detected, counted, and surfaced; audio thread never blocks (M9.1).
- [ ] *(full M9)* no drift over 10 min on separate input/output devices (M9.5/6).

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Resampler / drift loop blow the xrun budget on the audio thread | profile against the allocation + a CPU budget; fixed-ratio polyphase first; keep adaptive drift correction coarse |
| Hot-plug / drift are untestable without hardware | build a **mock Core Audio backend** (inject device-died + drift) early; gate real-hardware soak tests behind `AIUDIO_LIVE` |
| Channel-count change ripples through every node's `prepare()` | mechanical but broad — land it behind the existing tests (uniform-count graphs are the special case, stay green) |
| Scope creep into in-graph multi-rate / plugin host | explicitly out of scope (§2) — those are Phase 1 / M7 |
| Drift comp is a deep rabbit hole | defer M9.5/6 until a concrete multi-device need; aggregate devices (M4) cover the common case |

---

## References
- `docs/71-io-layer-milestones.md` §M9 (the milestone + acceptance); ADR-0004 (RT safety),
  ADR-0005 (swappable clock/backends), ADR-0008 (shared clock / aggregate devices),
  ADR-0009 (graph spine — the channel-count assumption M9.2 extends).
- `testing/README.md` (where M9's tests slot in); `include/aiudio/io/ring_buffer.hpp`
  (the graceful-degradation primitive); `src/io/coreaudio_backend.cpp` (the I/O path M9 hardens).

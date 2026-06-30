# 76 — True Multi-Source I/O: Project Plan (absorbs M9)

> **Last updated:** 2026-06-29 · **Goal:** N independent input sources *and* M output
> sinks — each on its own hardware clock — brought onto **one engine timeline**, composed
> in **one graph**, controllable from **Python**, all **RT-safe**. · **Status:** in
> progress — **the single-clock multi-source MVP is built and merged** (G10, G8, G9, M9.1, M11a,
> **M10** all in `main`): N sources → one graph (mix/route) → M sinks via per-stream lock-free
> rings. The **master-clock adapter** (live-feed the manager from any backend) and **M9.4**
> (mock backend + hot-plug/disconnect + device-xrun model) are 🟡 in review — the full live
> path now runs headlessly via the mock. Remaining = the **real Core Audio HAL hot-plug
> listener** (hardware-verified) and the productionization tail (M9.3 resampling, **M9.5
> off-clock drift**, M9.6 cross-clock). See the
> **[delivery plan / PR chain in §11](#11-delivery-plan--the-pr-chain-live-status)** for live status. (Post-Phase-0; the I/O parts cluster in Phase 3 productionization, the
> spine parts are general and can land earlier.)
>
> This plan **absorbs the M9 robustness/hardening plan** (formerly `docs/75`) as its
> Phase B, and **aligns** with: ADR-0008 (multi-input model), ADR-0009 (graph spine),
> ADR-0004/0005 (RT safety / swappable clock), ADR-0010 (control plane), `docs/71`
> (I/O M-milestones), `docs/74` (graph-spine G-milestones), and the `README` roadmap.
>
> Provenance (`CLAUDE.md` §8): **✓ Verified** = grounded in current code; **○ Background**
> = standard DSP/Core Audio knowledge to confirm.

---

## 1. Why this is more than M9 — the three-layer model

True multi-source I/O is the **composition** of three concerns that ADR-0008
deliberately keeps separate. M9 only owns the first:

| Layer | Concern | Owned by | Status |
|---|---|---|---|
| **Alignment** | put each source on one coherent clock (drift/resample), survive xruns/hot-plug | **M9** (Phase B) | planned |
| **Composition** | N backends + N SPSC rings; present N inputs / M outputs to the graph | **multi-source manager (M10)** + **multi-stream executor (G10)** | **not built** |
| **Mixing/routing** | combine/route the sources into signals | a **mixer node** (`SumNode` ✓) + **channel routing (G8)** | partial |

So **full M9 is necessary but not sufficient**: it makes each source *alignable and
robust*; the manager + multi-stream executor make them *composable*; the mixer/routing
nodes combine them. This plan builds all three.

### The hard facts it must respect (✓ Verified in code)
- The RT contract is **single-input/single-output**: `RenderCallback::process(const
  AudioBuffer& in, AudioBuffer& out, …)`, and the executor feeds **every `SourceNode` the
  same one `in`** (`for (SourceNode* s : sources) s->setExternalInput(&in)`). Multi-source
  therefore requires an **executor/IR change** (Phase D), not just I/O work.
- The SPSC `RingBuffer` is **one-producer/one-consumer** — never merge producers into one
  ring (ADR-0008 §2); each source gets its own ring.
- `AudioBuffer` already carries its own `numChannels`, and the executor already allocates
  **one buffer per output port** and walks topologically — so per-port channel counts
  (G8) ride existing structure.

---

## 2. End-state architecture

```
 sources (each = one clock = one backend, ADR-0008 §5)
   mic  ─▶ inBackend₀ ─▶ ring₀ ─┐
   iface─▶ inBackend₁ ─▶ ring₁ ─┤   ┌──────────────── Multi-Source Manager (M10) ───────────────┐
   tap  ─▶ inBackend₂ ─▶ ring₂ ─┼──▶│ one master clock drives the IOProc;                       │
   file ─▶ fileSource  ─▶ ring₃ ─┘   │ off-clock sources are drift-comp/resampled (M9) onto it; │
                                     │ presents named input streams + named output streams      │
                                     └───────────────┬───────────────────────────┬───────────────┘
                                                     ▼ (G10 multi-stream executor) ▼
                                       SourceNode[mic] SourceNode[iface] …      SinkNode[outA] SinkNode[outB]
                                                     └──▶  graph (mix via SumNode, route via G8) ──┘
                                                                                  │
   sinks  ◀─ outBackend₀ ◀─ ring  ◀──────────────────────────────────────────────┘
          ◀─ outBackend₁ ◀─ ring  ◀───────────────────────────────────────────────
```

Everything crossing a thread boundary is a lock-free ring or an atomic (ADR-0004); each
clock source is one backend (ADR-0008 §5); mixing is a graph node (ADR-0008 §4).

---

## 3. The governing principle (unchanged from M9)

> **The audio thread degrades gracefully and never blocks.** Detection, accounting, and
> recovery (drift correction, re-open, resize) happen off-thread; the callback only
> substitutes safe data (silence on underrun, drop on overrun) and moves on (ADR-0004).

---

## 4. Phases & milestones

Estimates are order-of-magnitude for one developer (same scale as `docs/71`/`docs/74`).
RT: 🟢 off-thread/compile-time · 🟡 light per-block · 🔴 real per-sample DSP on the audio
thread. **New milestone IDs** extend the existing M/G numbering.

### Phase A — Engine prerequisites (graph spine; reusable beyond multi-source)

**G8 — Per-port channel counts** · ✅ **merged (PR #15)** · 🟢 · ADR-0012 (extends ADR-0009)
Each port declares/infers its own channel width; a **channel-count propagation pass** in
`GraphExecutor::build()` sizes buffers per output port (✓ rides the existing per-port
allocation + topo walk); `Node::channelLayout(inCounts)→outCounts` (default: inherit/
broadcast); a mechanical `prepare()`-signature change so stateful nodes size state.
*Unlocks:* placing sources side-by-side as channels, and split/merge/down-up-mix routing.
*Acceptance:* a 2→1 downmix and a 1→2 pan render correctly offline (golden); `process()`
stays allocation-free.

**G9 — Node/edge latency reporting + delay compensation** · ✅ **merged (PR #16)** · 🟡 · ADR-0013
Add `latencyFrames()` to the node/backend contract (✓ Verified absent today) and a
graph-wide delay-compensation pass, so resampled/buffered/aligned sources stay
sample-aligned. *Reused later by* lookahead/FFT/neural nodes.
*Acceptance:* a delay-introducing node reports latency; two paths of differing latency
recombine in phase.

### Phase B — Source alignment & hardening (**= M9, absorbed in full**)

This is the former `docs/75`. M9 makes each individual source robust and clock-alignable.
RT/est/deps per sub-milestone; **do M9.1 first** (it makes every later live test trustworthy).

| ID | Deliverable | RT | Est | Deps |
|---|---|---|---|---|
| **M9.1** xrun/underrun policy — ✅ **merged (PR #17)** | detect (ring over/underrun counters ✓ + executor under-served-block counter ✓); substitute (silence on underrun, drop on overrun ✓); atomic `xrun_count` / `dropped_commands` telemetry ✓; `StreamConfig.XrunPolicy` knob ✓ (enforcement deferred). **Device-side HAL-timestamp gap + `kAudioDeviceProcessorOverload` detection deferred to M9.4** (needs the mock backend to test). | 🟡 | 2–3 d | — |
| **M9.2** channel mapping/routing | device↔graph mapping incl. **mono↔stereo**; within-graph routing/matrix nodes | 🟢 | 2–4 d | **G8** |
| **M9.3** boundary sample-rate conversion | fixed-ratio polyphase FIR (or libsamplerate/Speex ○) at the I/O edge; alloc-free; reports latency | 🔴 | 4–6 d | M9.1, **G9** |
| **M9.4** device hot-plug/disconnect + fallback — ✅ **in review (PR)** | The **model** is built + testable: an `AudioBackend` disconnect-handler + `disconnected()`/`xrunCount()`; a **`MockBackend`** (deterministic, manually-ticked) that injects disconnect + xrun so the live path + hot-plug + device-side xrun (the M9.1 deferral) are exercised headlessly. **Remaining (hardware-verified):** the real Core Audio HAL device-died listener wiring on the device backends. | 🟢 | 4–7 d | M9.1 |
| **M9.5** drift compensation (separate clocks) | adaptive resampling driven by a ring-fill control loop | 🔴 | 1.5–2.5 wk | M9.3 |
| **M9.6** multi-**device** (1-in + 1-out, separate clocks) | the stepping-stone to N sources: one input device + one output device, kept in sync | 🔴 | 1–1.5 wk | M9.5 |

> **Naming caution carried over:** M9.6 "multi-device" = *one* input + *one* output on
> separate clocks. It is **not** N sources — that's Phase C.

### Phase C — Multi-source transport (the ADR-0008 manager)

**M10 — Multi-source manager** · ✅ **merged (PR #19)** · 🟢 (transport off-thread) · ADR-0014 (makes ADR-0008 §5 concrete)
Implements ADR-0008 §5's deferred "future multi-source manager":
- **C1 — Per-source capture as named sources:** generalize the existing input/duplex/tap
  backends so each registers as a named source feeding **its own SPSC ring** (ADR-0008 §2).
- **C2 — The manager:** owns **N input backends + N rings** and **M output backends + M
  rings**; one **master clock** drives the engine IOProc; **off-clock sources are
  drift-compensated/resampled (Phase B) onto the master timeline**; exposes named input
  streams + named output streams to the executor.
- **C3 — Symmetric output side:** graph → M output sinks via per-sink rings/backends.
*Acceptance:* mic + a process tap + a file, on different clocks, all arrive sample-aligned
on the engine timeline; two outputs receive distinct signals; xrun-clean over a soak run.

### Phase D — Graph/executor multi-IO (graph spine)

**G10 — Multi-stream executor + source/sink binding** · ✅ **merged (PR #14)** — first of the chain · 🟢 (compile-time) · ADR-0011 (extends ADR-0009)
The IR/executor change that lets the graph *receive* N inputs and *drive* M outputs:
- Extend the executor entry point to take **named input/output streams** (a `stream-id →
  AudioBuffer` set) instead of a single `in`/`out`; **`SourceNode`/`SinkNode` carry a
  stream id** and bind to a specific source/sink (instead of all reading the one `in`).
- **Recommended design (b):** a multi-stream `process(inputs[], outputs[], …)` driven by
  the manager (C2). *(Alternative (a) — concatenate all sources into one wide buffer and
  select channel ranges — leans entirely on G8 and conflates sources with channels;
  rejected as the primary path because distinct clocks/rates map cleanly to distinct
  streams, per ADR-0008 §2. (a) remains available for the "all sources share a clock"
  fast path.)*
- **Composition:** mixing uses the existing `SumNode` (✓), routing uses **G8**.
*Acceptance:* a graph with two `SourceNode`s bound to two different live sources + a
`SumNode` mixes them; two `SinkNode`s drive two outputs — verified offline (deterministic,
two numpy inputs → two numpy outputs) and live.

### Phase E — Control plane / Python (extends ADR-0010)

**M11 — Input-side + multi-source Python bindings** · 🟢 · ~1–1.5 wk · no ADR
- **E1:** bind the input/duplex/tap backends — ✅ **merged (PR #18, M11a)**: `InputBackend`,
  `DuplexBackend`, `TapBackend` (+ `ProcessInfo`, `list_processes`) bound, GIL released on
  lifecycle, `running`/`latency_frames` telemetry. (Previously only the *output* `DeviceBackend`
  + offline were bound.) Live mic capture verified on hardware.
- **E2:** bind the multi-source manager — open N sources / M sinks, bind them to graph
  Source/Sink nodes, per-source telemetry (level, xruns, drift) — all via the existing
  lock-free control plane (ADR-0010).
- **E3:** a Python API to declare a multi-source topology.
*Acceptance:* from Python, open mic + a tap, mix them, monitor to two outputs, tweak gains
live — without touching the audio thread; the acceptance walkthrough notebook extended.

---

## 5. Master dependency graph

```
G8 (per-port channels) ─┐
                        ├─▶ M9.2 ─┐
M9.1 (xrun) ────────────┤         │
G9 (latency) ─▶ M9.3 ─▶ M9.5 ─▶ M9.6 (multi-device)         (Phase B = M9, alignment)
                        │         │
                        └────┬────┴─────────────▶ M10 (multi-source manager, Phase C)
                             │                         │
                        M9.4 ┘                         ▼
                                              G10 (multi-stream executor, Phase D)
                                                       │
                                                       ▼
                                              M11 (Python, Phase E)  +  SumNode mixing / G8 routing
                                                       │
                                                       ▼
                                          ✅ TRUE MULTI-SOURCE I/O
```
**Critical path:** G9 → M9.3 → M9.5 (drift) gates the *cross-clock* manager; G8 gates
routing; G10 gates the graph receiving N streams. M9.1, G8, G9 are independent and should
start first.

---

## 6. Alignment with the existing plans (reconciliation)

| Prior plan / decision | How this plan relates |
|---|---|
| **ADR-0008** (multi-input: per-source rings, one-backend-per-clock, aggregate-then-resample, mixing-is-a-node, "future multi-source manager") | This plan **implements** ADR-0008 — M10 *is* the deferred manager; Phase B *is* the alignment it requires; mixing stays a node. |
| **ADR-0009** (graph spine; single in/out; uniform channels) | Extended by **G8** (per-port channels) + **G10** (multi-stream executor). Both need a **superseding/extending ADR**. |
| **ADR-0004/0005** (RT safety / swappable clock) | Unchanged and binding: every new transport is off-thread rings/atomics; each clock = one backend. |
| **ADR-0010** (Python control plane) | Extended by **M11** (input-side + multi-source bindings) on the same lock-free hooks. |
| **`docs/71`** (I/O M0–M9) | **M9 absorbed here (Phase B)**; adds **M10** (manager) and **M11** (Python). M7 (plugin host) is unrelated/parallel. |
| **`docs/74`** (graph spine G1–G7) | Adds **G8/G9/G10** as spine extensions (this is their home). |
| **`README` roadmap** | Lands as a **Phase 3 — productionization** track (multi-source I/O); the spine prerequisites (G8–G10) are general and pullable into Phase 1's timeframe. |
| **Node-library list / per-port channels / latency** (earlier discussions) | G8 + G9 are exactly those engine prerequisites, promoted to milestones here; the mixer (`SumNode`) and routing nodes are the composition layer. |

---

## 7. New ADRs to write
1. **Per-port channel counts** (G8) — extends/supersedes ADR-0009's uniform-channel decision.
2. **Multi-stream executor / source-sink binding** (G10) — extends ADR-0009's single-in/out contract.
3. **Latency reporting + delay compensation** (G9) — new node/graph metadata + a compensation model.
4. **Multi-source manager clocking** (M10) — the N-backends/N-rings/master-clock model (relates to ADR-0008, makes its §5 concrete).
*(Added to the `adr/README.md` backlog.)*

---

## 8. Effort roll-up
- **Spine prerequisites (G8+G9):** ~1.5–2 wk — general-purpose, do early.
- **Phase B (M9):** minimum-viable ~2.5–4 wk; full (incl. drift M9.5 + multi-device M9.6) ~6–8 wk.
- **Phase C (M10):** ~2–3 wk.
- **Phase D (G10):** ~1–1.5 wk.
- **Phase E (M11):** ~1–1.5 wk.
- **Total to true multi-source I/O:** roughly **8–12 focused weeks** for a robust version
  (dominated by full M9's drift work). A *single-clock* multi-source MVP — N sources that
  already share a clock (e.g. one aggregate device's many channels), skipping drift — is
  far less: ~**G8 + G10 + M10(single-clock) + M11 ≈ 4–6 wk**.

---

## 9. Definition of done — true multi-source I/O
- [ ] N independent input sources (e.g. mic + tap + file) on different clocks arrive **sample-aligned** on one engine timeline (M9.3/M9.5 + M10).
- [ ] A single graph **binds distinct `SourceNode`s to distinct sources**, mixes/routes them (`SumNode` + G8), and drives **M distinct output sinks** (G10 + M10).
- [ ] `process()` remains **allocation-free / lock-free**; per-source xruns detected + counted; survives a source hot-unplug (M9.1/M9.4).
- [ ] The whole topology is **declarable and controllable from Python**, with per-source telemetry — audio thread untouched (M11).
- [ ] Covered by the test strategy: golden multi-input/output offline renders + a gated live multi-source soak test (`testing/`).

---

## 10. Risks
| Risk | Mitigation |
|---|---|
| Drift comp (M9.5) is a deep rabbit hole | ship the **single-clock multi-source MVP** first (no drift); add drift only for genuine cross-clock setups |
| Multi-stream executor (G10) changes the core contract | extend, don't fork: keep single-`in`/`out` `process()` as the 1-stream special case; land behind existing tests |
| Hot-plug/drift untestable without hardware | build a **mock Core Audio backend** (inject device-died + drift) — shared with the M9 test plan |
| Scope creep (in-graph multi-rate, plugin host) | explicitly out of scope: in-graph multi-rate → Phase 1; plugin host → M7 |
| `prepare()`-signature ripple (G8) touches every node | mechanical; uniform-count graphs are the special case → stays green |

---

## 11. Delivery plan — the PR chain (live status)

The roadmap above is delivered as a chain of stackable PRs, ordered so the offline
foundation lands first, then a demonstrable single-clock **live MVP**, then the hard
cross-clock work last. Each PR ships its own tests + docs + (where marked) ADR, and is
verified green before merge. **Status legend:** ✅ merged · 🟡 in review · ⬜ planned.

| # | Branch | Milestone — delivers | Status | Needs | Test layer |
|---|---|---|---|---|---|
| 1 | `feat/g10-multistream-executor` | **G10** — multi-stream executor (N in / M out streams; source/sink stream binding) | ✅ **merged (PR #14)** | main | offline |
| 2 | `feat/g8-per-port-channels` | **G8** — per-port channel counts + `DownmixNode`/`UpmixNode` (channel-width change) | ✅ **merged (PR #15)** | main | offline (golden) |
| 3 | `feat/g9-latency-pdc` | **G9** — node/edge latency reporting + delay compensation (ADR-0013) | ✅ **merged (PR #16)** | main | offline |
| 4 | `feat/m9-1-xrun-policy` | **M9.1** — xrun/underrun policy + telemetry (device-side HAL detection → M9.4) | ✅ **merged (PR #17)** | main | headless + RT-alloc |
| 5 | `feat/m11-input-bindings` | **M11a** — bind the input / duplex / tap backends to Python | ✅ **merged (PR #18)** | main | gated live |
| 6 | `feat/m10-multisource-manager` | **M10 (single-clock)** — manager (per-stream rings, one clock) + Python multi-source API → ⭐ **MVP** | ✅ **merged (PR #19)** | 1, 5 (+2, 4) | C++ + TSan + Python |
| — | `feat/m10-live-and-m9-4` | **Master-clock adapter** — `MasterClockAdapter` lets any backend's clock drive the manager (the live-feeding layer) | 🟡 **in review (PR)** | M10 | C++ + TSan + Python (mock) |
| 7 | `feat/m10-live-and-m9-4` | **M9.4 (core)** — `MockBackend` + hot-plug/disconnect + device-xrun model (the M9.1-deferred device xruns) | 🟡 **in review (PR)** | 4 | C++ + TSan + Python + mock |
| 8 | `feat/m9-3-resampler` | **M9.3** — boundary sample-rate conversion | ⬜ | 3, 4 | offline + live |
| 9 | `feat/m9-5-drift-comp` | **M9.5** — adaptive drift compensation | ⬜ | 8 | mock + soak |
| 10 | `feat/m9-6-cross-clock` | **M9.6 + M10(cross-clock)** — true cross-clock multi-device = the full goal | ⬜ | 9, 6 | mock + live soak |

**MVP cut-line** ⭐ — after **PR 6** you have demonstrable multi-source: N inputs on a
shared clock → mixed/routed in one graph → M outputs, from Python, RT-safe, *without* drift
comp. (PRs 1, 5, 6 + optionally 2, 4 — the `~4–6 wk` single-clock MVP of §8.)

**Dependency notes (parallelizable honestly):**
- Strict chains: `1 → 6 → 10`, `3 → 8 → 9 → 10`, `5 → 6`.
- Independent (branch off `main`, any order): PRs **1, 2, 3, 4, 5** — none depends on another;
  the table is a *recommended* linear order.
- **Done so far:** PR 1 (G10) ✅, PR 2 (G8) ✅, PR 3 (G9 latency/PDC) ✅, PR 4 (M9.1 xrun policy)
  ✅, PR 5 (M11a input bindings) ✅, **PR 6 (M10 multi-source manager) ✅ — all merged**. **The
  single-clock multi-source MVP is built**: N sources → one graph (mix/route, channel-change,
  PDC) → M sinks via per-stream lock-free rings, TSan-clean, with xrun telemetry. **Remaining:**
  the live device-master-clock feeding adapter, and the productionization tail — PR 7 (M9.4
  hot-plug, which also lands the device-side xrun detection deferred from M9.1), PR 8 (M9.3
  boundary resampling), PR 9 (**M9.5 off-clock drift** — the gate for true multi-device live),
  PR 10 (M9.6 cross-clock). *Note:* PR 2 delivered the channel-width engine + the down/up-mix
  nodes; the **device↔graph channel mapping** slice of M9.2 (mono↔stereo at the I/O boundary)
  is still open and folds naturally into the live-feeding adapter or a small PR of its own.
- Test infra: notebook execution now runs in the test interpreter (a throwaway kernelspec on
  `sys.executable`), so the suite is robust regardless of the global `python3` kernel.

---

## References
ADR-0004/0005/0008/0009/0010; `docs/71` (I/O milestones, M9 row → here), `docs/74`
(graph spine, G8–G10 extend it), `docs/60` (build order / gaps), `testing/README.md`
(verification), `include/aiudio/io/ring_buffer.hpp`, `src/io/coreaudio_duplex_backend.cpp`
(aggregate-device precedent), `src/graph/graph_executor.cpp` (the single-`in` feed this changes).

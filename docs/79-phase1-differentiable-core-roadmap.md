# 79 — Phase 1: Differentiable Core — Implementation Roadmap

> **Last updated:** 2026-07-01 · **Goal:** make the graph IR **differentiable and trainable** —
> a *third executor* (alongside real-time and offline) that runs the **same IR** through PyTorch
> autograd, so DSP-node parameters are learnable and neural models are first-class peers.
> Proven by gradient-based parameter optimization ("match a target") and closed by writing trained
> parameters **back into the C++ real-time graph**. · **Status:** **in progress — D0 landed**
> (the differentiable executor spine + node registry + C++↔torch parity harness + trivial nodes;
> ADR-0016/0017; the optional `aiudio.diff` / `aiudio[diff]` package). Phase 0 complete, Tier-1 DSP
> nodes landed. Research grounding is **✓ Verified** (from `docs/20`/`docs/50`); the remaining
> milestones (D1–D8) are a **design proposal** (○) until each ADR is locked at implementation.
>
> Extends the README **Phase 1 — Differentiable core** and [`docs/78`](78-node-library-roadmap.md)
> (Tier 3). *Why* the pillar exists: [`docs/50`](50-architecture-patterns.md) §3 and
> [`docs/00`](00-vision-and-scope.md). The hard problems: [`docs/20`](20-differentiable-dsp-and-neural-audio.md) §2.

---

## Contents
- [1. The one idea](#1-the-one-idea)
- [2. Where it sits — the third executor](#2-where-it-sits)
- [3. Foundation abstractions](#3-foundation-abstractions)
- [4. The hard problems (and how we tame them)](#4-the-hard-problems)
- [5. Milestones (D0–D8)](#5-milestones)
- [6. Dependency / sequencing graph](#6-dependency--sequencing-graph)
- [7. New ADRs this phase will spawn](#7-new-adrs)
- [8. Definition of done](#8-definition-of-done)
- [9. Risks specific to the differentiable core](#9-risks)
- [10. Delivery plan — the PR chain](#10-delivery-plan)
- [11. Effort roll-up](#11-effort-roll-up)
- [12. Reconciliation with existing docs & ADRs](#12-reconciliation)

---

## 1. The one idea

Phase 0 built **one typed IR** that two executors run identically: the **real-time** executor
(C++, on the audio thread) and the **offline** executor (C++, faster-than-real-time). Phase 1 adds
the **third executor demanded by invariant §4.3** — a **differentiable** one:

> **The same `Graph` IR, evaluated through an autodiff framework, so the whole graph is
> differentiable end-to-end and its parameters can be optimized against an audio objective.**

This is pillar 3 of the vision (ML-first). Its verified precedent (`docs/50` §3): a **DAG of audio
processors can be reverse-engineered from input/output pairs by making the processors *and* the
routing differentiable** (arXiv:2406.01049), and **Text2FX** tunes effect params against a
perceptual (CLAP) objective by gradient descent. Generalize both to the aiudio graph and you get
the Phase-2 punchline: *the agent proposes structure; gradient descent tunes the parameters.*
Phase 1 builds the gradient-descent half.

**The headline deliverable:** take a graph (say EQ → compressor), a target render, and **recover
the parameters by backprop** — then write them into the C++ real-time graph and hear the same
result live. That single slice exercises every foundation piece below.

---

## 2. Where it sits

Three hard rules constrain the whole design; getting them right is more than half the work.

1. **The differentiable executor is Python + PyTorch, and it never touches the audio thread**
   (ADR-0002, ADR-0004). Training is a **research/offline** activity: heavy, batched, GPU-friendly,
   allocation-happy — the exact opposite of the RT path. So Phase 1 adds **no** code to the audio
   thread. The RT core stays C++; the differentiable core is a peer executor in Python.
2. **One IR, not two.** The differentiable executor consumes the **same `Graph`** (via the
   bindings' introspection — `g.nodes()`, topology, per-node params), not a hand-maintained Python
   copy. The graph is the single source of truth; the diff executor is a *second interpreter* of
   it (like the offline backend is a second driver of the RT executor).
3. **The bridge is parameters, not audio.** ML-first → real-time is closed by **exporting trained
   parameters/coefficients back into the C++ graph** (`set_param` / coefficient setters) and
   verifying parity. Neural-model *deployment* to the RT thread (RTNeural/ONNX, ADR-0006) is a
   **Phase-3** concern; Phase 1 delivers the **training** path and the parameter round-trip.

```
  Phase 0 (done):   Graph IR ──► RT executor (C++, audio thread)
                             └──► Offline executor (C++, faster-than-RT)
  Phase 1 (this):   Graph IR ──► Differentiable executor (Python/PyTorch, off-thread)
                                    │  autograd end-to-end
                                    ▼
                            optimize params ──export──► back into the C++ Graph  ──► RT
```

---

## 3. Foundation abstractions

Get these five interfaces right before writing training loops.

1. **The differentiable executor (`DiffExecutor`).** Walks the compiled IR in the *same
   topological order* the C++ executor uses and evaluates each node's differentiable `forward()`
   over a batched tensor **`[batch, channels, frames]`** (float64 for `gradcheck`, float32 for
   training). Per-port "buffers" become tensors; autograd is the tape. Fan-out/mix mirror the
   graph edges (a `Sum`/`Mixer` node adds tensors). No RT constraints apply here.
2. **The node's dual face + registry.** Each node type gains a Python **diff implementation**
   `forward(x, params) -> y` (pure torch) alongside its C++ `process()`. A **registry** maps
   `type_name → diff class`. Each declares its **differentiability status** — `Full` /
   `Surrogate` (trainable via an approximation) / `NonDiff` (frozen; passes through with no grad) —
   the invariant §4.7 property, now *enforced* and surfaced in metadata.
3. **Learnable parameters + reparameterization.** Node params become `torch.nn.Parameter`s, but
   **constrained** so optimization stays well-posed: frequencies in **log space**, gains in dB,
   ratios via **softplus**, mix/pan via **sigmoid/tanh**. A thin mapping layer translates between
   the C++ **index-based** params (`docs/82` appendix) and named torch parameters.
4. **The parity harness (the "one IR" guarantee across the third backend).** For every node, the
   diff `forward()` must match the C++ `process()` within tolerance on the same input+params — a
   **golden parity test** per node. This is what keeps the two implementations honest and lets a
   trained torch model round-trip to C++ (D6). Where the coefficient math can be shared/derived
   identically, do so; where it can't (surrogate filters), document the intended divergence.
5. **Losses.** **Multi-resolution STFT loss** (the DDSP/neural-audio standard — via `auraloss` /
   `torchaudio`, *don't reinvent*), plus MSE/L1 for direct signal matching, and a hook for a
   **perceptual (CLAP) loss** to connect to Phase 2. (`docs/20` §7: multi-scale spectral loss is
   the workhorse — but see §4 on its blind spots.)

---

## 4. The hard problems

Differentiability is **not free** (`docs/20` §2). Each hazard has a chosen mitigation; the plan
budgets for them explicitly rather than discovering them mid-training.

| Hazard (✓ from `docs/20`) | Why it bites | Mitigation in this plan |
|---|---|---|
| **Oscillator frequency gradients are uninformative** (§2.1) | audio/spectral losses are non-convex in pitch; naïve descent won't estimate frequency | pitch-aware losses / **staged training** / self-supervised init; **don't** train pitch on bare multi-res STFT (D8 note) |
| **IIR / recursive filters impede autodiff** (§2.2) | direct-form biquad recursion → poor/unstable gradients | train through the **SVF form** (`SvfNode`) or **frequency-sampling**; declare direct-form biquad `Surrogate`/`NonDiff` and export SVF→biquad coeffs for RT (D2) |
| **Hard nonlinearities** (hardclip, quantization) | zero/undefined gradient | **straight-through estimator**; prefer smooth surrogates (`tanh`, `softclip`) for training (D3) |
| **Stateful recurrence** (envelopes, delays, feedback) | backprop-through-time is costly/unstable | **frame-based** formulation + **truncated BPTT**; fractional (interpolating) delay for a differentiable delay line (D1/D3) |
| **C++ ↔ torch numeric drift** | two implementations diverge silently | the **parity harness** (§3.4) gates every node in CI |
| **Cost** (training on audio is heavy) | long sequences × many params | **batching + GPU**, block/frame processing, mixed precision where safe |

---

## 5. Milestones

A **D-series** (Differentiable), parallel to Phase 0's G-series (graph) and M-series (I/O). Each
milestone ships with a **parity test** (vs C++ where applicable), **pytest**, and **reproducibility**
(seed + pinned deps).

| ID | Milestone | Delivers | Depends on |
|---|---|---|---|
| **D0** ✅ | Differentiable executor spine | `DiffExecutor` reads the IR + a diff-node registry; autograd source→sink; parity on a trivial graph | Phase 0 (IR, bindings, introspection) |
| **D1** | Differentiable **linear** DSP nodes | Gain, Sum, Mixer, Pan, ChannelMatrix, DcBlocker, (fractional) Delay — `forward` + gradcheck + parity | D0 |
| **D2** | Differentiable **filters** (the IIR problem) | `SvfNode` (trainable) + frequency-sampling; magnitude-match training; SVF→biquad export | D1 |
| **D3** | Differentiable **dynamics & nonlinearities** | Waveshaper (STE for hardclip), Compressor/Gate (smooth surrogate + truncated BPTT) | D1 |
| **D4** | **Losses + training harness** | multi-res STFT (auraloss) + MSE/L1; `Trainer` (data→fwd→loss→bwd→step→checkpoint); reproducible | D0 |
| **D5** | **Parameter optimization** ("match a target") — the headline slice | recover a graph's params from a target render by backprop (EQ+comp) | D1–D4 |
| **D6** | **Round-trip to real time** | export trained params/coeffs → C++ `Graph`; golden parity: trained-torch vs C++-RT render | D2, D5 |
| **D7** | **First neural node** | `NeuralNode` wrapping a torch `nn.Module`, trainable as a graph peer; TorchScript export; RT deploy stubbed (Phase 3) | D0, D4 |
| **D8** | **DDSP exemplar + eval** | harmonic+noise DDSP (or learned EQ-match) end-to-end example, small dataset, metrics; notebook | D4, D5 |

**D0 — Differentiable executor spine.** *Acceptance:* on `source→gain→sink`, `loss.backward()`
yields finite gradients w.r.t. the gain; the diff forward matches the C++ `process()` within
`1e-6`; the batched tensor convention `[batch, channels, frames]` is fixed and documented.

**D1 — Linear nodes.** *Acceptance:* each node's diff forward matches C++ within tolerance;
`torch.autograd.gradcheck` (float64) passes for every learnable param; the fractional `Delay`
line is differentiable w.r.t. delay time.

**D2 — Filters.** *Acceptance:* an `SvfNode` trains to match a target magnitude response (loss
↓ over N steps, stable); exporting SVF→biquad coefficients into the C++ `BiquadNode` reproduces
the trained response within tolerance (feeds D6). Direct-form biquad is declared `Surrogate`.

**D3 — Dynamics/nonlinear.** *Acceptance:* the waveshaper trains (drive/mix); the compressor's
diff forward matches C++ within tolerance and gradient flows through the envelope with bounded
BPTT; differentiability status per node is declared and tested.

**D4 — Losses + trainer.** *Acceptance:* a toy target converges; the run is **deterministic**
under a fixed seed; checkpoints save/restore params round-trip; CPU + (if available) GPU paths
both run.

**D5 — Parameter optimization.** *Acceptance:* given a target produced by *known* EQ+compressor
params, recover them from a random init to within tolerance by gradient descent; monotone-ish loss
decrease; this is the "brighten the vocal / match the tone" slice (README Phase 1).

**D6 — Round-trip.** *Acceptance:* a golden test — the trained torch render and the C++ `Graph`
(with the exported params) produce matching output within tolerance. Closes ML-first → RT.

**D7 — First neural node.** *Acceptance:* a small torch module (e.g. a tiny amp/tone model,
NAM-/RAVE-class in spirit) trains end-to-end **inside a graph** alongside DSP nodes; exports to
TorchScript; `realtime_capable=false` for now (RT deployment = Phase 3, ADR-0006).

**D8 — DDSP exemplar.** *Acceptance:* a runnable, documented end-to-end example (DDSP synth or
EQ-match) with a small dataset and reported metrics (multi-res STFT distance; CLAP hook); shipped
as a notebook + example, mirroring the Phase-0 cookbooks.

---

## 6. Dependency / sequencing graph

```
 Phase 0 (IR, node contract, bindings, param setters, WAV I/O)  ✅
        │
        ▼
       D0  differentiable executor spine
        │
        ├─────────────┬───────────────┐
        ▼             ▼               ▼
       D1 linear     D4 losses+trainer   (D4 also needs only D0)
        │             │
        ├──► D2 filters                │
        ├──► D3 dynamics               │
        └──────┬───────────────────────┘
               ▼
              D5 parameter optimization  ──►  D6 round-trip to RT
               │
   D0+D4 ──►  D7 first neural node
   D4+D5 ──►  D8 DDSP exemplar + eval
```

**Parallelizable:** D2 and D3 are independent once D1 lands; D4 can start right after D0; D7 needs
only D0+D4. **Critical path:** D0 → D1 → D2 → D5 → D6 (the differentiable-filter → optimize →
round-trip spine).

---

## 7. New ADRs

Written when each decision locks (at implementation, per `adr/README.md` §process). Candidate
numbers continue from **ADR-0015**:

- **ADR-0016 — Differentiable execution strategy.** A Python/PyTorch differentiable executor that
  interprets the same C++ `Graph` IR; the **dual node implementation** (C++ `process()` + torch
  `forward()`) governed by a **parity harness**; nodes declare `Full`/`Surrogate`/`NonDiff`
  differentiability status. *(Architecturally significant — a new executor backend + a
  cross-language implementation contract; extends ADR-0003/0009.)*
- **ADR-0017 — Autodiff framework = PyTorch.** The training substrate for the ML layer (a major
  dependency choice; ADR trigger). Rationale: dominant in neural audio (DDSP/RAVE/auraloss),
  natural fit for ADR-0002's Python ML layer, TorchScript/ONNX export paths toward RT (ADR-0006).
- **ADR-0018 (with D2) — Trainable-filter form.** SVF / frequency-sampling as the *trainable*
  filter representation, with SVF→biquad coefficient export for RT (resolves `docs/20` §2.2 in
  the codebase). *(May fold into ADR-0016.)*
- **ADR-0006 revisited (with D7):** ADR-0006 already covers RT neural *inference* (RTNeural inline
  / ANIRA off-thread). D7 adds the **training** side (torch) + **export**; note the split rather
  than superseding, unless deployment specifics force a new ADR.

The node contract already reserves "**differentiable parameters**" and an explicit
"**differentiability status**" (invariants §4.4/§4.7) — Phase 1 *enforces* what Phase 0 declared.

---

## 8. Definition of done

Phase 1 is "the differentiable core is built" when:

1. **The same IR runs on the differentiable executor** — `DiffExecutor` interprets a `Graph`
   built with the existing API; forward output matches the C++ executor within tolerance (**parity**
   for every differentiable node).
2. **DSP node parameters are trainable** — linear nodes (D1), trainable filters (D2), and dynamics
   /nonlinear nodes with declared status (D3), all gradient-checked.
3. **Gradient-based parameter optimization works on a real graph** — the "match a target" slice
   (D5) recovers known params from a random init.
4. **Trained parameters round-trip into the C++ RT graph** with matching output (D6) — ML-first →
   real-time is closed.
5. **At least one neural node trains as a first-class peer** (D7).
6. **A reproducible training harness + multi-res STFT loss + a DDSP exemplar** exist, tested and
   documented (D4/D8), mirroring the Phase-0 cookbooks.
7. **Docs/ADRs current** — README Phase-1 boxes ticked as milestones land; ADR-0016/0017 written;
   `docs/78` Tier-3 status advanced; a Phase-1 cookbook (the fourth in the `docs/81–83` series:
   "Differentiable & Trainable Graphs") added.

**Explicit non-goals for Phase 1** (deferred): the neural-model *zoo* (source separation, codecs,
generation — Phase 4); **RT neural deployment** (streaming/cached-conv, RTNeural/ONNX on the audio
thread — Phase 3); the **agent** (Phase 2). Phase 1 is the *core mechanism* + a convincing proof +
the *first* neural node.

---

## 9. Risks

- **Dual-implementation drift (C++ vs torch).** *Mitigation:* the parity harness gates every node
  in CI; share/derive coefficient math where possible; document intended divergences (surrogates).
- **Ill-conditioned / non-differentiable ops** (pitch gradients, IIR, hardclip, integer delay).
  *Mitigation:* the §4 table — staged/pitch-aware losses, SVF/freq-sampling, straight-through,
  fractional delay. Declare `NonDiff` honestly rather than pretend.
- **Stateful recurrence stability & cost.** *Mitigation:* frame-based + truncated BPTT; test
  gradient stability over long sequences.
- **Training performance.** *Mitigation:* batching, GPU, block processing; keep the differentiable
  executor firmly off the RT path so RT is never taxed.
- **Scope creep into Phase 3/4.** *Mitigation:* the non-goals above are explicit; D7 stubs RT
  deployment; the model zoo waits.
- **Reproducibility rot.** *Mitigation:* seeds + pinned ML deps + checkpoints from D4 onward
  (CLAUDE.md §6).

---

## 10. Delivery plan

A stacked PR chain, each gated on parity + tests (same discipline as the Phase-0/multi-source
chains). One milestone per PR unless trivially combinable:

| # | Branch (suggested) | Milestone | Gate |
|---|---|---|---|
| 1 | `feat/d0-diff-executor` | D0 executor spine + registry + parity harness | pytest + C++ parity + ADR-0016/0017 |
| 2 | `feat/d1-diff-linear-nodes` | D1 linear nodes | gradcheck + parity |
| 3 | `feat/d2-svf-trainable-filters` | D2 SVF/freq-sampling + export | train-to-target + parity + ADR-0018 |
| 4 | `feat/d3-diff-dynamics` | D3 waveshaper/compressor/gate | parity + BPTT gradient test |
| 5 | `feat/d4-losses-trainer` | D4 multi-res STFT + Trainer | determinism + checkpoint round-trip |
| 6 | `feat/d5-param-optimization` | D5 match-a-target slice | recovers known params |
| 7 | `feat/d6-roundtrip-to-rt` | D6 export → C++ parity | golden round-trip test |
| 8 | `feat/d7-neural-node` | D7 NeuralNode + TorchScript | trains as a peer; export |
| 9 | `feat/d8-ddsp-exemplar` | D8 example + eval + notebook | runs end-to-end; metrics reported |

---

## 11. Effort roll-up

Rough order-of-magnitude (single focused implementer); the differentiable-filter and round-trip
work dominate.

| Milestone | Est. |
|---|---|
| D0 executor spine + parity harness | 1.5–2.5 wk |
| D1 linear nodes | 1–1.5 wk |
| D2 trainable filters (SVF/freq-sampling + export) | 2–3 wk |
| D3 dynamics/nonlinear (surrogates + BPTT) | 2–3 wk |
| D4 losses + trainer | 1–1.5 wk |
| D5 parameter optimization slice | 1–1.5 wk |
| D6 round-trip to RT | 1–1.5 wk |
| D7 first neural node | 2–3 wk |
| D8 DDSP exemplar + eval | 1.5–2.5 wk |
| **Total** | **~13–20 wk** (core spine D0–D6 ≈ 9–13 wk) |

---

## 12. Reconciliation

| Prior decision / doc | How Phase 1 relates |
|---|---|
| **ADR-0002** (C++ RT core + Python ML layer) | Phase 1 *is* the ML layer's core; the differentiable executor is Python/torch, never on the audio thread. |
| **ADR-0003/0009** (one IR, node contract) | Phase 1 adds the **third executor** over the same IR and *enforces* the contract's differentiability fields; extended by candidate ADR-0016. |
| **ADR-0004** (audio thread sacred) | Untouched — training is off-thread/offline; the RT path gains no code. |
| **ADR-0006** (runtime-agnostic neural inference) | Covers RT *deployment* (Phase 3); D7 adds the *training* + export side. |
| **`docs/78`** (node-library roadmap) | Phase 1 = docs/78 **Tier 3** ("makes Tier 1 differentiable + adds neural peers"); this doc is its milestone-level plan. |
| **`docs/20`** (differentiable-DSP research) | Supplies the verified hazards (§4) and techniques (DDSP, SVF, multi-res STFT). |
| **`docs/50` §3** (differentiable rendering) | The architectural thesis + precedent (arXiv:2406.01049, Text2FX) this roadmap operationalizes. |
| **Phase 2** (agent) | Consumes Phase 1: "agent proposes structure → differentiable core tunes params" (`docs/40`, ADR-0010 hooks). |

---

**Cross-references:** README **Roadmap** (Phase 1); [`docs/00`](00-vision-and-scope.md) (vision);
[`docs/20`](20-differentiable-dsp-and-neural-audio.md), [`docs/50`](50-architecture-patterns.md) §3
(design); [`docs/78`](78-node-library-roadmap.md) (Tier 3); the usage cookbooks
[`docs/81`](81-pipeline-usage-patterns.md)/[`docs/82`](82-node-usage-patterns.md)/[`docs/83`](83-live-control-and-dynamic-graphs.md)
(a Phase-1 "Differentiable & Trainable Graphs" cookbook joins them at D8).

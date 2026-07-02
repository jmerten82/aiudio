# 79 — Phase 1: Differentiable Core — Implementation Roadmap

> **Last updated:** 2026-07-01 · **Goal:** make the graph IR **differentiable and trainable** —
> a *third executor* (alongside real-time and offline) that runs the **same IR** through PyTorch
> autograd, so DSP-node parameters are learnable and neural models are first-class peers.
> Proven by gradient-based parameter optimization ("match a target") and closed by writing trained
> parameters **back into the C++ real-time graph**. · **Status: ✅ COMPLETE — D0–D8 all landed.**
> The `DiffExecutor` auto-mirrors *any* graph (`param_value`/`node_config`/`sample_rate` getters +
> auto-read, `init_params` optional); the full DSP node library is differentiable — linear (D1),
> trainable filters (D2), dynamics/nonlinear/recursive (D3: Waveshaper, DcBlocker, Compressor,
> Gate, Delay via per-frame scans, cold-parity with C++). D4 = multi-res STFT loss + `fit`/
> checkpoint harness; D5 = parameter-match (`match_target`); D6 = round-trip to RT
> (`export_to_graph` — C++ render == trained-torch render); D7 = first neural node (a torch
> `nn.Module` as a graph peer, trained jointly with DSP, exported via `torch.export`); D8 = a DDSP
> `HarmonicSynth` exemplar (timbre match, multi-res-STFT-distance metric). **Next: Phase 2** (agent
> control plane) — see [README Roadmap](../../README.md#roadmap).
> (D0: the differentiable executor spine + registry + C++↔torch parity harness, ADR-0016/0017, the
> optional `aiudio.diff` / `aiudio[diff]` package; D1: the stateless linear nodes Mixer + Pan).
> Phase 0 complete, Tier-1 DSP nodes landed. Research grounding is **✓ Verified** (from
> `docs/theory/20`/`docs/theory/50`); the milestone work (D0–D8) is now **implemented and tested** (each ADR
> 0016/0017/0018 accepted at implementation).
>
> Extends the README **Phase 1 — Differentiable core** and [`docs/pipeline/78`](78-node-library-roadmap.md)
> (Tier 3). *Why* the pillar exists: [`docs/theory/50`](../theory/50-architecture-patterns.md) §3 and
> [`docs/00`](../00-vision-and-scope.md). The hard problems: [`docs/theory/20`](../theory/20-differentiable-dsp-and-neural-audio.md) §2.

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

This is pillar 3 of the vision (ML-first). Its verified precedent (`docs/theory/50` §3): a **DAG of audio
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
   the C++ **index-based** params (`docs/cookbooks/82` appendix) and named torch parameters.
4. **The parity harness (the "one IR" guarantee across the third backend).** For every node, the
   diff `forward()` must match the C++ `process()` within tolerance on the same input+params — a
   **golden parity test** per node. This is what keeps the two implementations honest and lets a
   trained torch model round-trip to C++ (D6). Where the coefficient math can be shared/derived
   identically, do so; where it can't (surrogate filters), document the intended divergence.
5. **Losses.** **Multi-resolution STFT loss** (the DDSP/neural-audio standard — via `auraloss` /
   `torchaudio`, *don't reinvent*), plus MSE/L1 for direct signal matching, and a hook for a
   **perceptual (CLAP) loss** to connect to Phase 2. (`docs/theory/20` §7: multi-scale spectral loss is
   the workhorse — but see §4 on its blind spots.)

---

## 4. The hard problems

Differentiability is **not free** (`docs/theory/20` §2). Each hazard has a chosen mitigation; the plan
budgets for them explicitly rather than discovering them mid-training.

| Hazard (✓ from `docs/theory/20`) | Why it bites | Mitigation in this plan |
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
| **D1** ✅ | Differentiable **stateless linear** nodes | Mixer (per-input learnable gains) + Pan (equal-power 1→2, learnable) — `forward` + gradcheck + parity. *Scope refined:* DcBlocker + Delay are recursive → moved to **D3**; ChannelMatrix needs channel-layout introspection → deferred (small follow-up). | D0 |
| **D2** ✅ | Differentiable **filters** (the IIR problem) | `DiffBiquad` — design-param (log-freq/softplus-Q/gain_dB) magnitude-response training + design→biquad coeff export (ADR-0018). *Time-domain* recursive filtering in-graph → D3. | D1 |
| **D3** ✅ | Differentiable **dynamics, nonlinear & recursive** | Waveshaper (tanh/softclip FULL; hardclip STE), DcBlocker (recursive scan), **Compressor/Gate** (linked-peak envelope, per-frame scan / truncated BPTT), **Delay** (feedback recurrence) — all match C++ (cold parity ≤ 2e-6) and auto-mirror via introspection. Limits (documented): gate `threshold_db` is a hard knee (non-diff); delay time is integer (non-diff). | D1, enabler |
| **D4** ✅ | **Losses + training harness** | `MultiResolutionSTFTLoss` (torch.stft — dep-free, standard formula) + `mse`/`l1`; `fit` (data→fwd→loss→bwd→step) + `seed_everything` + `save/load_checkpoint`. | D0 |
| **D5** ✅ | **Parameter optimization** ("match a target") — the headline slice | `match_target` recovers a graph's params from a target **render** by backprop through a multi-node audio-domain graph (gain → waveshaper → compressor); learned params push to C++. | D1–D4 |
| **D6** ✅ | **Round-trip to real time** | `export_to_graph` writes trained params → C++ via `set_param`; golden parity: **C++ render == trained-torch render** (atomic/stateful compared cold; smoothed after settling). Closes ML-first → RT. | D2, D5 |
| **D7** ✅ | **First neural node** | C++ `NeuralNode` placeholder (identity in RT — inference is Phase 3) + `NeuralDiffNode` wrapping an injected torch `nn.Module`; trains **jointly with DSP nodes**; exports via `torch.export` (→ ONNX/ExecuTorch, ADR-0006). | D0, D4 |
| **D8** ✅ | **DDSP exemplar + eval** | `HarmonicSynth` (harmonic + filtered-noise, fixed f0) trains to match a target **timbre** via multi-res STFT loss — recovers the spectral envelope (`ex_ddsp_synth_match`). Metric: STFT distance (CLAP = Phase-2 hook). | D4, D5 |

**D0 — Differentiable executor spine.** *Acceptance:* on `source→gain→sink`, `loss.backward()`
yields finite gradients w.r.t. the gain; the diff forward matches the C++ `process()` within
`1e-6`; the batched tensor convention `[batch, channels, frames]` is fixed and documented.

**D1 — Stateless linear nodes.** ✅ *Delivered:* `MixerNode` (per-input learnable gains,
width-preserving) and `PanNode` (equal-power 1→2 — a channel-width change, G8). *Acceptance met:*
diff forward matches C++ within tolerance (bit-exact for construction-set params; within the
`SmoothedValue` float32 steady state, ~2e-5, for `set_param`-driven gains), `gradcheck` (float64)
passes, and gradients match analytic. *Scope note:* the recursive `DcBlocker` (one-pole) and
`Delay` (feedback) fit D3's scan/BPTT machinery, so they moved there; `ChannelMatrix` needs the
node's channel layout exposed (a small introspection follow-up) and is deferred.

**D2 — Filters.** ✅ *Delivered:* `DiffBiquad` (`aiudio.diff.filters`) — a differentiable RBJ
biquad in reparameterized **design-param** space (log-freq, softplus-Q, gain_dB). *Acceptance met:*
formula parity with the C++ `BiquadNode::design()` (analytic |H| vs the impulse-response FFT of
`add_biquad_*`); `fit_magnitude` recovers a target response by gradient descent (loss ↓ ≥100×,
freq within 10%, gain within 0.6 dB); exporting the trained coeffs via `add_biquad_coeffs`
reproduces the response within tolerance (feeds D6). Trains through the analytic **magnitude
response** — the IIR-autodiff workaround (`docs/theory/20` §2.2, ADR-0018). Running a filter *inside* a
differentiable graph forward (time-domain recursion) lands with the recursive nodes in **D3**.

**D3 — Dynamics, nonlinear & recursive.** ✅ *Delivered (complete):* `WaveshaperDiffNode`
(tanh/softclip FULL; hardclip straight-through SURROGATE), `DcBlockerDiffNode` (recursive one-pole
scan), and the stateful set — `CompressorDiffNode` (linked-peak detector → gain computer →
attack/release envelope), `GateDiffNode` (open/close envelope), `DelayDiffNode` (feedback
recurrence `ring[t]=x[t]+fb·ring[t−delay]`) — each a differentiable **per-frame scan** (truncated
BPTT). All **auto-mirror the graph** via the introspection enabler. *Acceptance met:* C++↔torch
parity for every node (stateful ones compared cold, `warmup=0`: compressor ≤ 2.2e-6, gate ≤ 2.4e-7,
delay 0), gradients flow through the recursions, differentiability status declared + tested.
*Honest limits (documented):* the gate's `threshold_db` sits only in the hard `where` condition so
it's non-differentiable (a soft knee would sacrifice exact parity); the delay time is integer
(a fractional/interpolating delay for a trainable delay-*time* is a later refinement).

**D4 — Losses + trainer.** ✅ *Delivered:* `MultiResolutionSTFTLoss` (spectral convergence +
log-magnitude L1 over several FFT sizes, on `torch.stft` — dep-free; FFT sizes clamped for short
blocks) + `mse`/`l1`; the `fit` loop, `seed_everything`, and `save_checkpoint`/`load_checkpoint`
(the trained params export to C++ — D6). *Acceptance met:* a gain converges under both MSE and the
STFT loss; the run is **deterministic** under a fixed seed (identical loss history); checkpoints
round-trip params exactly. (`auraloss` is a drop-in alternative; we kept the extra to just torch.)

**D5 — Parameter optimization.** ✅ *Delivered:* `match_target(model, x, target, …)` recovers a
graph's parameters from a target **render** by gradient descent through the differentiable graph.
*Acceptance met:* on a multi-node audio-domain graph (gain → waveshaper → compressor) the loss
drops ≥ 100× and the render matches; on a cleanly-identified case (waveshaper drive/mix, and the
`ex_diff_param_match` example's gain+drive+mix) the params are recovered near-exactly
(e.g. 0.801/2.992/0.603 vs 0.8/3.0/0.6 via the STFT loss). The "brighten/shape the tone to match"
slice (README Phase 1). *Note:* EQ (filter) matching uses D2's magnitude-domain path — a
time-domain filter node *inside* an audio-domain graph is the D6/enabler refinement flagged earlier
(entangled params match the render, not always the exact values).

**D6 — Round-trip.** ✅ *Delivered:* `export_to_graph(diff_executor, executor)` writes every
trained param back into the C++ graph via `set_param` (each diff node declares its
`{c++ index: value}` via `export_params()`). *Acceptance met:* golden tests — the C++ render
matches the trained-torch render within tol (atomic/stateful nodes like gain→compressor compared
**cold**, `warmup=0`; smoothed nodes like gain→waveshaper after the smoother **settles**), and the
end-to-end `match_target → export_to_graph → C++ process` reproduces a target. Closes ML-first → RT.

**D7 — First neural node.** ✅ *Delivered:* a C++ `NeuralNode` (a first-class graph peer,
identity pass-through in RT — `config()` reports `realtime_capable=0`) + a Python `NeuralDiffNode`
that runs an injected torch `nn.Module` (`DiffExecutor(modules={id: module})`, auto-registered so
its weights train). *Acceptance met:* a tiny per-sample MLP (NAM-flavored) trains **inside a graph
alongside a DSP gain** — both receive gradients and jointly learn to match a target nonlinearity;
the trained module **exports via `torch.export`** (the modern, non-deprecated replacement for
TorchScript; lowers to ONNX/ExecuTorch → RTNeural/ANIRA/LibTorch, ADR-0006). RT inference of the
model stays Phase 3 (the C++ node is a placeholder). Neural weights deploy by export, not
`set_param` (so `export_params` is empty for the neural node).

**D8 — DDSP exemplar.** ✅ *Delivered:* `aiudio.diff.HarmonicSynth` — a differentiable
harmonic + filtered-noise synth (the DDSP additive model) at a **fixed f0**, with learnable
per-harmonic amplitudes + a noise gain. It trains, via the multi-res STFT loss (D4) + `fit`, to
**match a target timbre**, recovering the spectral envelope (harmonic-amplitude error ≲ 2e-3 on a
1/n target; STFT distance collapses ~300×). Shipped as `examples/python/ex_ddsp_synth_match.py`
with the reported metric (multi-res STFT distance). *Pitch is fixed by design* — a multi-res STFT
loss is poor at pitch (`docs/theory/20` §2.1), so f0 is not learned by naive descent; learning *timbre* at
known pitch is exactly the loss's strength, and pitch-aware / staged training is a Phase-2+ concern.
A perceptual **CLAP-embedding** distance is the noted Phase-2 metric hook (`docs/theory/40`).

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
- **ADR-0018 — Trainable-filter form** ✅ **(Accepted, with D2).** Design-parameter
  (reparameterized: log-freq/softplus-Q/gain_dB) magnitude-response training + design→biquad
  coefficient export for RT — resolves `docs/theory/20` §2.2 in the codebase.
- **ADR-0006 revisited (with D7):** ADR-0006 already covers RT neural *inference* (RTNeural inline
  / ANIRA off-thread). D7 adds the **training** side (torch) + **export**; note the split rather
  than superseding, unless deployment specifics force a new ADR.

The node contract already reserves "**differentiable parameters**" and an explicit
"**differentiability status**" (invariants §4.4/§4.7) — Phase 1 *enforces* what Phase 0 declared.

---

## 8. Definition of done

**✅ All met (D0–D8 landed, PRs #39–#48).** Phase 1 is "the differentiable core is built" when:

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
7. **Docs/ADRs current** — README Phase-1 boxes ticked; ADR-0016/0017/0018 accepted; `docs/pipeline/78`
   Tier-3 status advanced; and the Phase-1 cookbook
   [`docs/cookbooks/84 — Differentiable & Trainable Graphs`](../cookbooks/84-differentiable-and-trainable-graphs.md) added
   (the fourth in the `docs/cookbooks/81–83` cookbook series).

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
| **`docs/pipeline/78`** (node-library roadmap) | Phase 1 = docs/pipeline/78 **Tier 3** ("makes Tier 1 differentiable + adds neural peers"); this doc is its milestone-level plan. |
| **`docs/theory/20`** (differentiable-DSP research) | Supplies the verified hazards (§4) and techniques (DDSP, SVF, multi-res STFT). |
| **`docs/theory/50` §3** (differentiable rendering) | The architectural thesis + precedent (arXiv:2406.01049, Text2FX) this roadmap operationalizes. |
| **Phase 2** (agent) | Consumes Phase 1: "agent proposes structure → differentiable core tunes params" (`docs/theory/40`, ADR-0010 hooks). |

---

**Cross-references:** README **Roadmap** (Phase 1); [`docs/00`](../00-vision-and-scope.md) (vision);
[`docs/theory/20`](../theory/20-differentiable-dsp-and-neural-audio.md), [`docs/theory/50`](../theory/50-architecture-patterns.md) §3
(design); [`docs/pipeline/78`](78-node-library-roadmap.md) (Tier 3); the usage cookbooks
[`docs/cookbooks/81`](../cookbooks/81-pipeline-usage-patterns.md)/[`docs/cookbooks/82`](../cookbooks/82-node-usage-patterns.md)/[`docs/cookbooks/83`](../cookbooks/83-live-control-and-dynamic-graphs.md)
(a Phase-1 "Differentiable & Trainable Graphs" cookbook joins them at D8).

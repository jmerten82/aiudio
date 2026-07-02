# 78 — Node-library roadmap (DSP + neural peers)

> **Last updated:** 2026-06-30 · **Goal:** grow the graph's node palette from the spine
> primitives into a real music-production + research library, in three tiers, leaning on the
> enablers now in `main` (per-port channels **G8**, latency/PDC **G9**, multi-stream **G10**,
> the live param queue + atomic telemetry, graph edit/introspection). Every node honours the
> **node contract** (ADR-0003/0009): `process()` is RT-safe (ADR-0004), and it declares
> `realtime_capable`, `latencyFrames()`, channel layout, and (Phase 1) differentiability.
> **Status:** **Tier 1 is implemented and merged** (this doc's first table); Tiers 2–3 are
> planned.

---

## Principle

All real features live in C++ nodes; Python composes them (ADR-0002). New nodes favour:
**(a)** music-production essentials, **(b)** clean **agent targets** (named, bounded params the
LLM can set), **(c)** differentiability where feasible (Phase 1). Feedback effects are
self-contained nodes with internal state (the graph is DAG-only); a graph-level unit-delay for
true cycles is deferred.

Continuous gain-like params are **click-free** via `SmoothedValue` (a one-pole glide driven by
the lock-free control queue). Live control is `GraphExecutor.set_param(node, index, value)`;
each node documents its param indices.

---

## Tier 1 — music-production essentials + the agent showcase · ✅ **implemented**

Classic DSP, RT-safe, several differentiable; the canonical "brighten the vocal / compress the
drums / pan it" agent gestures. All bound to Python (`Graph.add_*`).

| Node | Ports | What | Enabler | Params (set_param index) |
|---|---|---|---|---|
| `OscillatorNode` | 0→1 | sine/saw/square/triangle generator | — | 0=freq 1=amplitude |
| `NoiseNode` | 0→1 | white / pink noise | — | 0=amplitude |
| `BiquadNode` (peaking, low/high shelf) | 1→1 | parametric-EQ family (one band) | live queue | 0=freq 1=q 2=gain_db |
| `ParametricEqNode` | 1→1 | **multi-band EQ in one node** — cascaded bands from a `(type,freq,q,gain_db)` list | live queue | band·3 + {0=freq,1=q,2=gain_db} |
| `CompressorNode` | 1→1 | compressor / limiter, **look-ahead → reports latency** | **G9 PDC** | 0=threshold_db 1=ratio 2=attack 3=release 4=makeup |
| `GateNode` | 1→1 | noise gate / downward expander | — | 0=threshold 1=attack 2=release 3=range_db |
| `DelayNode` | 1→1 | delay + feedback + mix (internal feedback) | — | 0=delay_frames 1=feedback 2=mix |
| `WaveshaperNode` | 1→1 | tanh/soft/hard saturation (**differentiable**) | — | 0=drive 1=mix |
| `PanNode` | 1→1(2ch) | equal-power mono→stereo pan | **G8** | 0=pan |
| `StereoWidthNode` | 1→1 | mid/side width | — | 0=width |
| `MixerNode` | N→1 | weighted mix (per-input gain) | — | i = gain for input i |
| `ChannelMatrixNode` | 1→1(M ch) | routing/mix matrix in→out | **G8** | out·inCh+in = cell gain |
| `DcBlockerNode` | 1→1 | one-pole DC/rumble remover | — | — |

*Note:* trim / mute / phase-invert are just `GainNode` (gain / 0 / −1), so no separate nodes.

---

## Tier 2 — spectral & convolution (the payoff of G9 latency) · ⬜ planned

These introduce latency, which PDC (G9) absorbs. **Needs an FFT primitive** (hand-rolled
radix-2 or a vendored kiss/pffft) — the gating enabler for the tier.

| Node | What | Needs |
|---|---|---|
| `StftNode` / overlap-add framework | windowed FFT → process → IFFT | FFT |
| `ConvolutionNode` (partitioned FIR) | IR reverb + linear-phase EQ | STFT/conv |
| `ReverbNode` (FDN / Freeverb) | algorithmic reverb (internal feedback) | — |
| `SpectralGateNode` | classic pre-neural denoise | STFT |
| Analysis: `LoudnessNode` (LUFS), true-peak, spectrum | extend `MeterNode` (mean-square only today) | atomic telemetry ✓ |

---

## Tier 3 — differentiable & neural peers (the Phase-1 vision) · ◑ **partly landed** (Phase 1, `aiudio.diff`)

Where the framework's thesis (DSP + neural as one graph) gets proven. **Phase 1 (D0–D8) delivered
the differentiable + training side** in the optional `aiudio.diff` executor (torch, off-thread):
the whole Tier-1 library now has a differentiable, C++-parity form; a first neural node trains as a
graph peer; and a DDSP synth exemplar matches a target timbre. **RT deployment** of neural models
(and the heavier codec/separation nodes) remains Phase 3/4.

| Node | What | Status |
|---|---|---|
| Trainable filters | differentiable-friendly form | ✅ Phase 1 · D2 — `DiffBiquad`: design-param + **frequency-domain magnitude** training (`docs/20`: direct-form IIR has poor gradients); coeffs export to the C++ `BiquadNode` (ADR-0018) |
| DDSP synth (harmonic + filtered noise) | the canonical differentiable additive model | ✅ Phase 1 · D8 — `aiudio.diff.HarmonicSynth`, timbre match via multi-res STFT (learned reverb → later) |
| `NeuralNode` wrapper | a torch model as a first-class node | ◑ Phase 1 · D7 — trains as a graph peer + `torch.export` deploy path; **RT inference** (streaming/cached-conv, RTNeural/ANIRA/LibTorch — ADR-0006) is **Phase 3**. C++ node is an identity placeholder for now. |
| Codec node (EnCodec/DAC) | encode/decode to RVQ tokens | ⬜ Phase 4 — unlocks LLM/token workflows |
| Source separation / denoise | Demucs / neural | ⬜ Phase 4 — pooled, off-thread (ANIRA pattern), `realtime_capable=false` |

---

## Enablers still needed (do before the tiers that depend on them)

1. **Parameter smoothing** — ✅ landed with Tier 1 (`SmoothedValue`); biquad coefficient
   interpolation (for click-free EQ sweeps) is a refinement.
2. **An FFT primitive** — gates all of Tier 2.
3. **The neural runtime wiring** (ADR-0006 LibTorch/ONNX abstraction) — Tier 3 is a subsystem,
   not "just a node."
4. **Graph-level feedback** (a unit-delay node the executor permits in a 1-sample cycle) — only
   if a use case needs cross-node feedback; Tier 1/2 feedback effects use internal state.
5. **Named live-param setters / param metadata** — today control is index-based `set_param`;
   exposing per-node param names (for the agent + UIs) is a small follow-up.

---

## Alignment with existing plans
- Extends `docs/74` (graph spine G1–G7) and the README **Phase 1 — differentiable core**
  ("classic DSP nodes … as differentiable peers"). Tier 1 delivers the *classic* nodes; Tier 3
  makes them differentiable + adds neural peers.
- Honours ADR-0003/0009 (node contract), ADR-0004 (RT safety), ADR-0006 (runtime-agnostic
  neural), ADR-0012/0013 (per-port channels / latency) — no new ADR for Tier 1 (additive nodes
  under the existing contract); Tier 3's neural-runtime choice will warrant one.

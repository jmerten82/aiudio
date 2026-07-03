# 60 — Gaps & Opportunities

What does **not** yet exist, where an AI-native framework can **uniquely win**,
the **design risks**, and a suggested **build order**. This is the strategic
synthesis of the whole dossier.

> **Provenance key:** **✓ Verified** = grounded in the cited deep-research pass.
> **○ Background / synthesis** = reasoned from the evidence + established
> knowledge; confirm before betting on it.

---

## 1. The headline finding ✓

> **The building blocks all exist; the unification does not.** The state of the
> art supplies differentiable DSP, real-time neural inference, streamable codecs,
> and PyTorch→DAW deployment — but **no single system unifies classic DSP +
> neural models as first-class peers, across both real-time and offline, under
> one differentiable graph driven by an agent.** That unification is aiudio's
> thesis and its moat.

Two specific voids the research **could not fill** (strong signal they're open):

1. **The NL→DSP-graph control plane** ✓ — no verified evidence of a working
   system that compiles natural language into an *editable DSP/neural signal
   graph*. The closest precedents are **adjacent, not the whole**: Text2FX
   (text→*effect params* via CLAP+diff-DSP) and WavCraft (LLM→*pipeline of
   task-specific models*). Nobody has shown the full "intent → graph edits →
   tuned graph" loop.
2. **One engine, DSP+neural as true peers, real-time *and* offline** ✓ — every
   existing tool privileges one side (DSP hosts bolt on neural plugins; ML
   frameworks aren't RT-safe; deployment SDKs wrap *one model*, not a *graph*).

---

## 2. Where aiudio can uniquely win

### 2.1 The verifiable agent control plane ✓ (synthesis)
The reason an agent over audio can be made *reliable* — where free-form code
generation can't — is that **the graph is a typed, validatable, differentiable
compilation target**:
- The agent emits **typed graph edits** (validatable, diffable, undoable) — not
  free text. (`40-*` §5)
- Continuous params are set by **differentiable / embedding-guided optimization**
  (Text2FX pattern ✓), not LLM guesswork.
- The **differentiable graph is its own verifier**: render → score against a
  CLAP-style perceptual objective → self-correct.
This three-part loop (typed actions + gradient tuning + perceptual verification)
is a genuinely novel, defensible design, and it falls *out* of pillars 1+3 — no
one else has all three pieces in place to do it.

### 2.2 "Train it, then run it live" continuity ✓
Because **one IR feeds real-time, offline, and differentiable executors** (`50-*`),
a user (or the agent) can **train/optimize a graph offline and deploy the same
graph real-time** — with the streaming/cached-conv export path (RAVE/BRAVE ✓)
making non-causal-trained models causal. Today this is a manual, expert,
multi-tool process (PyTorch → TorchScript → Neutone → DAW). aiudio makes it one
artifact.

### 2.3 Neural + classic as peers in one optimizable graph ✓
The **differentiable mixing-graph** result ([arXiv:2406.01049](https://arxiv.org/pdf/2406.01049))
proves a *whole DAG of processors* can be optimized by gradients. Combine that
with neural nodes as peers and you can do things no current tool can: e.g.
"match this reference mix" → gradient-search a hybrid classic+neural graph;
"make my synth sound like X" → MIDI-DDSP-style hierarchical handles the agent can
turn.

---

## 3. Design risks & hard problems (ranked) ✓

| # | Risk | Severity | Evidence | Mitigation |
|---|---|---|---|---|
| 1 | **Agent reliability** — no proven NL→graph copilot exists | **High** (it's the differentiator *and* least de-risked) ✓ | `40-*`; verified gap | Typed IR action space; gradient-tuned params; perceptual verifier loop; prototype a thin slice first |
| 2 | **Real-time neural latency** — best models are non-causal/slow | **High** ✓ | `30-*` | Streaming/cached-conv export (RAVE/BRAVE ✓); ANIRA off-thread pool ✓; enforce `realtime_capable` per node |
| 3 | **Differentiable-DSP stability** — uninformative freq gradients; IIR autodiff | **Medium-High** ✓ | F2; arXiv:2404.07970; 2012.04572 | Pitch-aware/perceptual losses; custom backward passes; declare per-node diff status |
| 4 | **The one-IR-four-masters tension** — RT vs offline vs diff vs editable | **Medium** ○ | `50-*` §1 | "One IR, many backends" separation; static-edit-then-recompile |
| 5 | **Python↔C++ boundary** — GIL/alloc vs convenience | **Medium** ○ | `50-*` §5 | Serialize graph to neutral IR + Torch/ONNX; no Python on audio thread; nanobind |
| 6 | **Scope / "boil the ocean"** — the vision is large | **Medium** ○ | `00-*` | Vertical slices; lean on existing libs (don't rebuild torchaudio/JUCE/ANIRA) |

> **Honest caveat (✓):** the research that grounds risks #1 is *absence of
> evidence*, which is not evidence of absence. A targeted second research pass on
> NL-to-DSP agents (and on the uncovered topics in §5) is warranted before
> committing heavily.

---

## 4. Suggested build order (synthesis ○)

A de-risking sequence — prove the thesis cheaply before building the cathedral.

**Phase 0 — Graph spine.** A typed graph IR + a simple eager executor in
Python, with the node contract (`process` + differentiable params + RT metadata).
A handful of classic nodes (gain, biquad EQ, reverb) + one neural node (a small
RAVE-class or NAM model). *Goal: DSP and neural as peers in one graph.*

**Phase 1 — Differentiable slice.** Make the graph differentiable end-to-end;
reproduce a **Text2FX-style** "brighten the vocal" → EQ-node tuned by gradient
against a CLAP objective. *Goal: prove the verifiable-tuning mechanism (the
agent's micro-layer) on the smallest possible example.*

**Phase 2 — Agent macro-layer.** LLM emits typed graph edits over the Phase-0
node catalog (WavCraft-style orchestration); params tuned by Phase-1 mechanism;
render→score→retry loop. *Goal: end-to-end "intent → graph → result", the
differentiator, validated.*

**Phase 3 — Real-time core.** C++ executor with the two node classes (RTNeural
inline + ANIRA off-thread pool); streaming/cached-conv export so a Phase-0/2
graph runs live; VST3/CLAP host integration. *Goal: the same graph runs
real-time.*

**Phase 4 — Breadth.** More nodes (separation, more neural effects, codec node),
richer agent tools, datasets/eval harness, offline generation path.

> Phases 1–2 are the **highest-information, lowest-cost** experiments: they test
> the riskiest, most differentiating claim (the verifiable agent) without needing
> the C++ real-time core yet.

---

## 5. What still needs research ✓ (uncovered by this pass)

The verified pass explicitly did **not** cover these requested topics. They are
filled with **○ background** in the docs but warrant their own verified pass
before relying:

- [ ] **NL-to-DSP-graph agents** end-to-end (the #1 differentiator) — deepest gap.
- [ ] **Source separation** SOTA & real-time feasibility (Demucs/Spleeter/UMX).
- [ ] **Neural vocoders** (HiFi-GAN) & **generative** models (MusicGen, Stable
      Audio) — real-time vs offline placement.
- [ ] **Classic-framework architectures** in depth (Faust algebra, SC server,
      Web Audio scheduling) as IR design input.
- [ ] **Python+C++ interop** benchmarks (nanobind vs pybind11; LibTorch
      boundary costs).
- [ ] **Datasets** (MUSDB18 et al.) & **evaluation metrics** (SI-SDR, ViSQOL,
      FAD, MUSHRA/MOS) — the benchmarking harness.
- [ ] **Quantization** & embedded deployment specifics.

---

## 6. One-paragraph thesis (for the README / pitch)

> Every piece of an AI-native audio framework already exists in isolation —
> differentiable DSP makes classic and neural blocks trainable peers (DDSP),
> real-time-safe engines run neural nets on the audio thread (RTNeural/ANIRA),
> streamable models hit sub-10 ms latency (RAVE/BRAVE), codecs tokenize audio for
> LLMs (SoundStream/Mimi), and SDKs ship PyTorch models to DAWs (Neutone). What
> nobody has built is the **unification**: one differentiable signal graph where
> classic DSP and neural models are first-class peers, which runs both real-time
> and offline, and which an **LLM agent can author and tune from natural
> language** — using the graph itself as a verifiable compilation target. That is
> **aiudio**.

# aiudio — Vision & Scope

> **aiudio**: an AI-native digital audio processing framework.

This document captures the product vision and the scoping decisions that anchor
all subsequent research and design work. It is derived from the founding scoping
conversation, not from external research — the research documents in this folder
(`10-*` through `60-*`) are what ground the vision in the current state of the art.

---

## 1. One-line vision

A framework where **classical DSP and neural models are first-class, composable
peers in a single signal graph**, where an **LLM agent/copilot can author and
edit that graph from natural language**, and where the whole thing is built
**ML-first** (differentiable, trainable, deployable) — usable for both
**real-time** and **offline** audio work.

## 2. What "AI-native" means here

"AI-native" is a deliberately loaded term. For aiudio it means three things at
once, not just one:

| Pillar | Meaning | Why it matters |
|---|---|---|
| **Hybrid classic + neural graph** | Filters, FFT, dynamics, EQ, reverb and neural blocks (separation, denoise, neural synthesis, timbre transfer, neural FX) are the *same kind of node*. The graph engine doesn't privilege one over the other. | Most existing tools bolt a neural model onto a classic DSP host (or vice versa). A native peer model lets the agent and the optimizer reason over the whole graph. |
| **Agent / copilot control plane** | An LLM translates intent ("remove the reverb, brighten the vocal, glue the bus") into concrete graph edits and parameter settings. | This is the differentiating UX. The graph is the *compilation target* of natural language. |
| **ML-first dev workflow** | Differentiable DSP, training/fine-tuning loops, dataset tooling, and a model deployment/inference runtime are core, not add-ons. | The framework is meant to *create and adapt* models, not only run pre-baked ones. |

> Note: we are explicitly **not** choosing the narrow "neural models are just
> DSP blocks" interpretation alone. The hybrid + agent + ML-workflow trio is the
> vision.

## 3. Scoping decisions (locked)

These were decided in the founding conversation and are treated as fixed
constraints for the research and the first architecture pass.

### 3.1 AI's role — *Hybrid + Agent + ML-workflow* (all three)
The framework must support traditional DSP and neural models as first-class
peers, expose an LLM agent control plane, and provide an ML-first development
workflow (train/fine-tune/deploy). See `theory/40-ai-agents-for-audio.md` and
`theory/20-differentiable-dsp-and-neural-audio.md`.

### 3.2 Domain — *Music production first, plus general audio research*
Primary target is music production (mixing, mastering, effects, stem
separation, instrument/synth tooling). Secondary target is a domain-agnostic
research substrate. Speech/voice and pure real-time-interactive (games/AR/VR)
are **not** primary for v1, though the architecture should not preclude them.

### 3.3 Latency — *Both real-time and offline, configurable*
The engine must support:
- **Real-time mode**: streaming/block-based processing, low latency
  (target single-digit-to-low-tens of ms), suitable for plugin-host and
  embedded contexts. This is the hardest constraint and shapes the core.
- **Offline/batch mode**: quality-over-latency, file-based rendering, large
  non-causal models permitted.

Configurability between these modes is a first-class design requirement, not a
fork. See `theory/30-realtime-neural-inference.md`.

### 3.4 Implementation — *Python (research/ML) + C++ (real-time core)*
- **C++** for the real-time-safe audio core, graph engine, and plugin hosting.
- **Python** for the research/ML layer: model authoring, training, the agent
  orchestration, and high-level scripting.
- Interop (pybind11/nanobind) and the boundary between the two is itself a
  design problem. See `theory/50-architecture-patterns.md`.

## 4. Non-goals (for v1)

- A full DAW with a **timeline/arrangement** UI. aiudio is a *framework/engine*,
  not an end-user linear arranger (though it could power one). *Note:* a **visual
  signal-graph editor** in the browser **is in scope** from Phase 2 (see `docs/pipeline/85`) —
  that's a node/flow workbench over the live engine, not a timeline DAW.
- Speech-first features (TTS/STT/voice-conversion) as the primary product.
- Mobile/embedded-only deployment as the first target.
- Replacing PyTorch/JAX as a general ML framework — aiudio sits *on top of*
  established ML runtimes.

## 5. The hard problems (tracked from day one)

These recur throughout the research and are the design risks to keep front of
mind:

1. **Real-time neural latency** — most high-quality neural audio models are
   non-causal and/or too slow for <20 ms block processing. Bridging this is the
   central engineering challenge. (`theory/30-realtime-neural-inference.md`)
2. **Differentiable-DSP stability** — making classic DSP blocks differentiable
   and numerically stable enough to train through (filters, feedback, FFT).
   (`theory/20-differentiable-dsp-and-neural-audio.md`)
3. **Agent reliability** — getting an LLM to produce *correct, musically sane*
   graph edits, with verification/guardrails. (`theory/40-ai-agents-for-audio.md`)
4. **Python↔C++ boundary** — real-time safety (no GIL, no allocations, no
   locks on the audio thread) vs. Python's convenience. (`theory/50-architecture-patterns.md`)
5. **Graph engine semantics** — eager vs lazy, push vs pull, static vs dynamic
   graphs; how to support both real-time and differentiable execution from one
   IR. (`theory/50-architecture-patterns.md`)

## 6. Document map

Docs are organized into three folders (full index: [`docs/README.md`](README.md)):

| Area | Where | Contents |
|---|---|---|
| **This file** | `00-vision-and-scope.md` | vision, locked decisions, non-goals |
| **Theory & research** | [`theory/`](theory/) | the SOTA dossier (`10` landscape · `20` differentiable/neural audio · `30` RT neural inference · `40` AI agents · `50` architecture patterns · `60` gaps) + fundamentals primers (`73` digital-audio encoding · `75` how ADCs/DACs work) |
| **Pipeline** | [`pipeline/`](pipeline/) | implementation plans, milestones, capabilities, phase roadmaps (`70`–`80`, `85`) — incl. the I/O layer, graph spine, node library, Phase 1 (differentiable core) and Phase 2 (agent workbench) roadmaps |
| **Cookbooks** | [`cookbooks/`](cookbooks/) | runnable recipe guides (`81` topologies · `82` nodes · `83` live control · `84` differentiable/trainable graphs) |
| **References** | `90-references.md` · `_research-report.md` | consolidated cited source list + raw deep-research provenance |

---

*Status: foundational. Written from the founding scoping decisions. The
research docs that follow validate, challenge, and extend this vision against
the current state of the art.*

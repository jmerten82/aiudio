# 40 — AI Agents & Copilots for Audio

The framework's **differentiating pillar (2):** an LLM control plane that compiles
natural-language intent ("remove the reverb, brighten the vocal") into concrete
DSP/neural graph edits and parameter settings.

> **Provenance key:** **✓ Verified** = cited from the deep-research pass.
> **○ Background** = established knowledge, confirm before relying.
>
> **⚠️ This is the thinnest-evidenced area of the whole dossier.** The verified
> research found strong precedents for *adjacent* pieces (text→FX params,
> LLM→pipeline orchestration) but **no single working system that compiles
> natural language into an editable DSP/neural signal graph**. That absence is
> precisely aiudio's clearest differentiating opportunity — and its biggest open
> research risk. See `60-gaps-and-opportunities.md`.

---

## 1. The two precedents that matter most ✓

### 1.1 Text2FX — natural language → audio-effect parameters
[arXiv:2409.18847](https://arxiv.org/abs/2409.18847) · ICASSP 2025 ·
Northwestern + Adobe.

The **single most direct published precedent** for aiudio's control plane.
Text2FX *"leverages **CLAP** embeddings and **differentiable** digital signal
processing to control audio effects, such as **equalization and reverberation**,
using **open-vocabulary natural-language** prompts"* — e.g. "make this
in-your-face and bold", brightness/reverb-style prompts.

**How it works (the pattern aiudio should study):**
- A **CLAP** (Contrastive Language-Audio Pretraining) joint text-audio embedding
  scores how well processed audio matches the text prompt.
- The effects are **differentiable DSP** (EQ, reverb), so the system can
  **optimize effect parameters by gradient descent** to maximize CLAP alignment
  with the prompt — *no LLM required for the parameter search itself*.

> **Why this is huge for aiudio:** it shows the NL→DSP-control problem can be
> attacked as **differentiable optimization against a multimodal embedding**,
> not only as LLM tool-calling. This is a genuinely different (and verifiable)
> architecture for "brighten the vocal" — and it composes perfectly with
> aiudio's differentiable graph (`20-*`, `50-*`).

### 1.2 WavCraft — LLM as orchestrating control plane
[arXiv:2403.09527](https://arxiv.org/abs/2403.09527).

WavCraft *"leverages the in-context learning ability of the LLM"* to **decompose
a user's natural-language instruction into multiple sub-tasks, each dispatched to
a particular task-specific audio model.** This is the **NL→pipeline /
tool-orchestration** pattern: the LLM is a planner/router over a toolbox of audio
models, not a DSP engine itself.

> **Why this matters for aiudio:** it validates the **agent-as-orchestrator**
> design — the LLM plans and routes; specialized models/DSP do the work. This is
> the natural fit for aiudio's pillar 2: the agent emits **graph edits / tool
> calls**, the engine executes them.

---

## 2. Two complementary architectures for the control plane

Synthesizing §1, there are **two viable designs** — and aiudio likely wants
**both**, layered:

| | **A. LLM-as-orchestrator** (WavCraft-style) ✓ | **B. Differentiable optimization** (Text2FX-style) ✓ |
|---|---|---|
| LLM role | Plan: decompose intent → graph edits / tool calls | Pick objective + initial graph; gradient does the tuning |
| Strength | Flexible, compositional, handles "do X then Y" | Verifiable, grounded in audio, no hallucinated params |
| Weakness | Can hallucinate; params hard to get right | Needs differentiable blocks + a good embedding/objective |
| aiudio fit | The **macro** layer: build/edit the graph | The **micro** layer: *tune* a block to match intent |

**Proposed aiudio synthesis:** the LLM agent decides *which nodes* and *how
they connect* (structure); a differentiable / embedding-guided optimizer sets
*continuous parameters* to match the perceptual intent (Text2FX-style). CLAP (or
a successor multimodal audio embedding) is the shared "does this sound like what
they asked for?" judge. This split directly mitigates the agent-reliability risk
(LLMs are bad at precise numeric params; gradient descent is good at it).

---

## 3. Symbolic / MIDI & hierarchical control ✓/○

- **MIDI-DDSP** ✓ ([arXiv:2112.09312](https://arxiv.org/pdf/2112.09312), see
  `20-*` §3) — not an "agent", but the key precedent for **layered, interpretable
  control handles** (notes → performance → synthesis). An agent needs handles at
  multiple altitudes; MIDI-DDSP shows how to expose them.
- **Symbolic-music LLMs / agents** (○ background): a broad active area
  (text→MIDI, chord/arrangement assistants, music-theory tool-use). Relevant to
  aiudio's *composition*-adjacent features but secondary to the DSP-graph control
  plane.

---

## 4. The general LLM-agent toolkit aiudio inherits ○ (background)

> Background — established agent-engineering practice, not from the verified pass.

The control plane is, mechanically, a **tool-using LLM agent**. The mature
patterns aiudio should adopt:
- **Tool/function calling** over a typed catalog of graph operations
  (`add_node`, `connect`, `set_param`, `separate_stems`, `analyze`, …).
- **A typed graph IR as the action space** — the agent edits a validated data
  structure, not free-form code → bounds errors, enables undo/diff.
- **Verification loop** — render → measure (CLAP score, loudness, spectral
  stats) → self-correct. The differentiable/embedding layer (§2B) *is* this
  verifier.
- **Grounding via analysis tools** — give the agent ears: feature extractors,
  loudness/LUFS, spectral descriptors, stem detection, so "brighten" maps to a
  measurable target.
- **MCP / tool servers** — exposing aiudio's operations as a tool server makes
  the engine drivable by *any* capable model.

---

## 5. The hard problem: agent reliability ✓ (named) / ○ (mitigations)

The verified pass explicitly flags **agent reliability** as an open hard problem
and found **no evidence** of a robust NL-to-graph audio copilot. Concretely:

1. **Numeric precision** — LLMs set bad continuous params. → **Mitigation:** push
   param-setting to differentiable/embedding optimization (§2B), not the LLM.
2. **Musical/perceptual correctness** — "glue the bus" must do something *musically
   sane*. → **Mitigation:** constrain the action space to validated nodes +
   sensible ranges; verify by measurement.
3. **Hallucinated structure** — invalid graphs. → **Mitigation:** typed IR with
   schema validation; reject-and-retry; the engine is the source of truth.
4. **Determinism & reproducibility** — same prompt, predictable result. →
   **Mitigation:** record the emitted graph diff as the artifact; the *graph*, not
   the prose, is what's saved/replayed.

> **Strategic read:** the reason this works for aiudio specifically is that the
> **graph is a verifiable compilation target.** Unlike free-form code generation,
> every agent action is a typed edit to a structure the engine can validate and
> the differentiable layer can score. That is the moat (`60-*`).

---

## 6. What this means for aiudio

1. **Build the control plane as two layers** (✓): LLM-as-orchestrator for
   structure (WavCraft) + differentiable/embedding optimization for parameters
   (Text2FX). CLAP-style embedding as the shared perceptual judge.
2. **Make a typed graph IR the agent's action space** (○ best-practice) so every
   edit is validatable, diffable, undoable, replayable.
3. **Treat the differentiable graph as the agent's verifier** (✓ synthesis) —
   this is the single biggest reliability lever and a natural consequence of
   pillar 1 + pillar 3.
4. **This is the part to prototype earliest** — it's the differentiator *and* the
   least de-risked. A thin vertical slice ("brighten the vocal" → EQ node tuned
   via CLAP) would validate the whole thesis cheaply.

See `60-gaps-and-opportunities.md` for why this is the opportunity, and `50-*`
for the graph IR that makes it tractable.

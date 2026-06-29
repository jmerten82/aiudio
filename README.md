# aiudio

**An AI-native digital audio processing framework** — where classical DSP and
neural models are first-class, composable peers in one signal graph, an LLM agent
can author and edit that graph from natural language, and the whole thing is
built ML-first (differentiable, trainable, deployable) for both real-time and
offline use.

> **Status:** 🌱 Pre-alpha — foundation & design phase. The research dossier and
> design are complete; implementation starts at the I/O layer. No public API yet.
> **Last updated:** 2026-06-28.

---

## Table of contents
- [What is aiudio](#what-is-aiudio)
- [The three pillars](#the-three-pillars)
- [Why aiudio (the gap)](#why-aiudio-the-gap)
- [How it works](#how-it-works)
- [Architecture](#architecture)
- [Roadmap](#roadmap)
- [Repository structure](#repository-structure)
- [Documentation](#documentation)
- [Architecture Decision Records](#architecture-decision-records-adrs)
- [Getting started](#getting-started)
- [Tech stack & key decisions](#tech-stack--key-decisions)
- [Development & best practices](#development--best-practices)
- [Prior art & acknowledgements](#prior-art--acknowledgements)
- [License](#license)

---

## What is aiudio

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

Primary target: **music production**, plus a general audio-research substrate.

## The three pillars

1. **Hybrid signal graph** — filters, FFT, dynamics, EQ, reverb *and* neural
   blocks (separation, denoise, neural synthesis, timbre transfer, neural FX) are
   the *same kind of node*. The engine privileges neither.
2. **Agent / copilot control plane** — an LLM compiles intent ("remove the
   reverb, brighten the vocal") into typed graph edits; continuous parameters are
   tuned by differentiable / embedding-guided optimization, not LLM guesswork.
3. **ML-first developer workflow** — differentiable DSP, training/fine-tuning,
   datasets, and a model deployment/inference runtime are core, not add-ons.

## Why aiudio (the gap)

The deep-research pass (see [`docs/`](docs/)) confirmed: **the building blocks all
exist; the unification does not.** No single system treats DSP + neural as
first-class peers, across real-time *and* offline, under one differentiable graph
driven by an agent. The two clearest open opportunities:

- **A verifiable NL→DSP-graph control plane** — no working system compiles natural
  language into an *editable signal graph* today. The graph being a typed,
  validatable, differentiable compilation target is what makes the agent
  *reliable* (unlike free-form code generation).
- **One engine, DSP+neural peers, real-time + offline** — train/optimize a graph
  offline, then deploy the *same* graph live via streaming export.

See [`docs/60-gaps-and-opportunities.md`](docs/60-gaps-and-opportunities.md).

## How it works

**Three key concepts:**

- **The node contract.** Every node — classic DSP or neural — exposes
  `process(block) → block`, **differentiable parameters**, and metadata
  (`realtime_capable`, latency, causal?, sample-rate). Honor the contract and the
  whole graph is composable *and* trainable end-to-end. *(DDSP, extended.)*
- **One IR, many backends.** A single typed, serializable graph IR runs under
  multiple executors: a **real-time** (block, no-alloc) executor, an **offline**
  executor, and a **differentiable** executor for training — and is edited by the
  **agent**.
- **One duplex I/O callback, swappable clock.** The engine has a single real-time
  entry point `process(in, out, frames, time)`, driven by an output-device IOProc
  (standalone), a plugin host (VST3/AU/CLAP), or an offline pump.

## Architecture

```
┌──────────────────── Python (research / control layer) ─────────────────────┐
│  Agent (LLM) ─edits─▶ Graph IR builder ─trains─▶ Differentiable executor      │
│      │                    │  serialize (IR + Torch/ONNX artifacts)            │
└──────┼────────────────────┼───────────────────────────────────────────────────┘
       │ control-rate edits  │  (nanobind / LibTorch boundary — no Python on RT)
       ▼ (lock-free queue)   ▼
┌──────────────────── C++ (real-time core) ──────────────────────────────────────┐
│  I/O layer  ─drives─▶  Compiled schedule ─▶ RT executor (block, no-alloc)        │
│   (devices / taps /                          ├─ inline nodes  (RTNeural pattern) │
│    host / files)                             └─ pooled nodes  (ANIRA pattern)    │
│  Host integration: VST3 / AU / CLAP (JUCE) · Neutone / nn~ interop               │
└──────────────────────────────────────────────────────────────────────────────────┘
```

- **C++** owns the real-time-safe core (graph executor, scheduler, I/O, plugin
  hosting). **No Python, no allocation, no locks** on the audio thread — ever.
- **Python** owns research/ML: model authoring & training, the agent, dataset
  tooling, offline rendering, high-level scripting.
- Details: [`docs/50-architecture-patterns.md`](docs/50-architecture-patterns.md).

## Roadmap

High-level phases (synthesis of
[`docs/60-*` build order](docs/60-gaps-and-opportunities.md) and the
[I/O milestones](docs/71-io-layer-milestones.md)). Checkboxes are the living
status — **kept current as we go**.

### Phase 0 — Foundations *(in progress)*
- [x] Deep-research dossier & design docs (`docs/`)
- [x] I/O layer foundation plan (`docs/71-*`) & macOS capture plan (`docs/70-*`)
- [x] **I/O layer** — duplex device capture+playback, full-duplex clock (M0–M4)
  *(M0 spikes; M1 core; M2 output; M3 input; M4 full-duplex — all ✅ merged)*
- [ ] **Graph spine** — typed IR + eager executor + the node contract *(ADR-0009 + `docs/74`; G1 🔜 IR + node contract — in review; G2–G6 next)*
- [ ] First end-to-end: capture → trivial graph (gain/meter) → playback, live

### Phase 1 — Differentiable core
- [ ] Differentiable end-to-end graph execution
- [ ] Classic DSP nodes (gain, biquad EQ, reverb) as differentiable peers
- [ ] First neural node (RAVE-/NAM-class) under the same contract
- [ ] "Brighten the vocal" slice — EQ node tuned by gradient vs a CLAP objective
- [ ] Process-tap & offline/file backends (I/O M5–M6) *(M5 🔜 system/per-app capture — in review)*

### Phase 2 — Agent control plane *(the differentiator)*
- [ ] Typed graph-edit action space (`add_node`/`connect`/`set_param`/…)
- [ ] LLM orchestrates structure; differentiable layer tunes parameters
- [ ] Render → measure (CLAP/loudness/spectral) → self-correct loop

### Phase 3 — Real-time productionization
- [ ] C++ RT executor: inline + off-thread pooled neural nodes
- [ ] Streaming / cached-conv export (non-causal → causal)
- [ ] Plugin-host backend + VST3/CLAP packaging (I/O M7)

### Phase 4 — Breadth
- [ ] Source separation, more neural FX, neural-codec node
- [ ] Datasets + evaluation harness (SI-SDR, ViSQOL, FAD, MUSHRA/MOS)
- [ ] Offline generation path (diffusion/transformer)

> Milestone-level detail and acceptance criteria live in
> [`docs/71-io-layer-milestones.md`](docs/71-io-layer-milestones.md) and
> [`docs/60-gaps-and-opportunities.md`](docs/60-gaps-and-opportunities.md).

## Repository structure

```
aiudio/
├── README.md              ← this file (vision, roadmap, status)
├── CLAUDE.md              ← guidance for Claude Code: invariants, best practices, protocol
├── adr/                   ← Architecture Decision Records (why we chose what we chose)
│   ├── README.md          ← ADR process, lifecycle, index
│   ├── template.md        ← copy this for a new ADR
│   └── NNNN-*.md          ← one file per decision (append-only)
├── docs/                  ← research dossier + design + implementation plans
│   ├── README.md          ← documentation index
│   ├── 00-vision-and-scope.md
│   ├── 10..60-*.md        ← research dossier (landscape → gaps)
│   ├── 70-macos-audio-capture-plan.md
│   ├── 71-io-layer-milestones.md
│   └── 90-references.md
├── examples/              ← M0 Python I/O spikes + cpp/ M1 usage examples
├── include/aiudio/io/     ← aiudio-io public headers (M1)
├── src/io/                ← aiudio-io implementation (M1)
├── tests/                 ← C++ unit tests (CTest)
└── CMakeLists.txt         ← C++ build (aiudio-io library + tests)
```

## Documentation

Start at [`docs/README.md`](docs/README.md). The dossier is grounded in a
fact-checked research pass; each doc marks provenance: **✓ Verified** (cited) vs
**○ Background** (confirm before relying).

## Architecture Decision Records (ADRs)

Load-bearing architecture decisions are recorded as **ADRs** in [`adr/`](adr/) so
the project stays consistent as it grows. `/docs` explains how the system works
*today*; `/adr` records *why it got that way* — append-only, one decision per
file. Process and full index: [`adr/README.md`](adr/README.md).

Accepted so far:

| # | Decision |
|---|---|
| [0001](adr/0001-record-architecture-decisions.md) | Use ADRs to record architecture decisions |
| [0002](adr/0002-cpp-realtime-core-python-ml-layer.md) | C++ real-time core + Python research/ML layer |
| [0003](adr/0003-hybrid-graph-one-ir-node-contract.md) | Hybrid graph: one typed IR, many backends, universal node contract |
| [0004](adr/0004-realtime-safety-audio-thread.md) | Real-time safety: the audio thread is sacred |
| [0005](adr/0005-io-duplex-callback-swappable-clock.md) | I/O: one duplex callback, swappable clock |
| [0006](adr/0006-runtime-agnostic-neural-inference.md) | Runtime-agnostic neural inference (inline + off-thread) |
| [0007](adr/0007-macos-coreaudio-io.md) | macOS audio I/O via Core Audio (HAL + process taps) |
| [0008](adr/0008-multi-input-and-full-duplex-clocking.md) | Multi-input & full-duplex clocking (shared clock, aggregate devices, per-source ring buffers) |
| [0009](adr/0009-graph-spine-architecture.md) | Graph spine — IR, node model, and eager executor |

> **Significant decisions require an ADR.** See `CLAUDE.md` §9 for when to write
> one and how it ties into keeping the docs current.

## Getting started

**Build the C++ core + run tests** (macOS; `brew install cmake` if needed):

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Optionally build with sanitizers (the ring buffer is verified race-free under both):

```bash
cmake -S . -B build-tsan -DAIUDIO_SANITIZE=thread          && ctest --test-dir build-tsan
cmake -S . -B build-asan -DAIUDIO_SANITIZE=address,undefined && ctest --test-dir build-asan
```

**Run the C++ usage examples** (no audio device needed — see [`examples/cpp/README.md`](examples/cpp/README.md)):

```bash
./build/examples/cpp/ex_render_callback
./build/examples/cpp/ex_offline_render_wav out.wav && afplay out.wav
```

**Run the M0 I/O spikes** (Python):

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r examples/requirements.txt
python examples/m0_sine_out.py --list-devices
python examples/m0_sine_out.py --device Kanto                       # play a tone
python examples/m0_passthrough.py --device-in Sennheiser --device-out Kanto
```

See [`examples/README.md`](examples/README.md) and
[`docs/71-io-layer-milestones.md`](docs/71-io-layer-milestones.md).

## Tech stack & key decisions

| Area | Choice | Rationale |
|---|---|---|
| RT core language | **C++20** (clang 21) | real-time safety, plugin/DSP/ML-runtime ecosystem |
| Research/ML language | **Python 3.11+** | PyTorch/JAX, agent, scripting |
| Interop | **nanobind** | low-overhead C++↔Python bindings |
| Build | **CMake** + **scikit-build-core** | C++ + Python packaging |
| RT neural inference | **RTNeural** (inline) · **ANIRA** (off-thread pool) | proven RT-safe patterns |
| ML runtime(s) | **LibTorch / ONNX Runtime** (abstracted) | don't marry one runtime |
| Audio I/O (macOS) | **Core Audio** (HAL + process taps) | native, low-latency, per-app capture |
| Plugin hosting | **JUCE** (VST3/AU) + **CLAP** | industry standards + open modern format |
| First platform | **macOS** | dev target; portable core by design |

Locked decisions: [`docs/00-vision-and-scope.md`](docs/00-vision-and-scope.md) §3.

## Development & best practices

Coding standards, the **real-time-safety rules**, and the document-maintenance
protocol live in [`CLAUDE.md`](CLAUDE.md). The cardinal rule, repeated
everywhere: **the audio thread is sacred — no allocation, no locks, no syscalls,
no Python on it, ever.**

## Prior art & acknowledgements

aiudio stands on: **DDSP** (Magenta), **RAVE / nn~ / cached_conv** (IRCAM ACIDS),
**RTNeural** (J. Chowdhury), **ANIRA** (TU Berlin), **Neutone SDK**,
**SoundStream / Mimi** (Google / Kyutai), **Text2FX** & **WavCraft**. Full
citations in [`docs/90-references.md`](docs/90-references.md).

## License

TBD — to be chosen before first public release (note: VST3/JUCE/CLAP have
distinct licensing implications; see `docs/30-*` §7).

# aiudio

**An AI-native digital audio processing framework** — where classical DSP and
neural models are first-class, composable peers in one signal graph, an LLM agent
can author and edit that graph from natural language, and the whole thing is
built ML-first (differentiable, trainable, deployable) for both real-time and
offline use.

> **Status:** 🌱 Pre-alpha. **Phase 0 foundation complete:** the I/O layer
> (capture / system + per-app taps / full-duplex / file, M0–M6) and the graph spine
> (typed IR + executor + node library, runs live & offline, live-editable, with
> **Python bindings** and a **Python control plane** that drives a *running* pipeline
> as a frontend, G1–G7) are implemented and tested, with a documented C++ + Python
> [testing strategy](testing/README.md) (CI-green). Remaining I/O — M7 (plugin host),
> M9 (hardening) — is not a Phase-0 gate. **Phase 1 (differentiable core) complete:**
> the optional `aiudio.diff` third executor (PyTorch, off-thread) runs the same IR
> through autograd — the full DSP node library is differentiable, with losses + a
> trainer, parameter-match against a target, a round-trip that writes trained params
> back into the C++ real-time graph, a first neural node, and a DDSP synth exemplar
> (D0–D8, [`docs/pipeline/79`](docs/pipeline/79-phase1-differentiable-core-roadmap.md)).
> **Phase 2 (agent control plane & visual workbench) in progress — 4 of 5 releases shipped:** a
> browser **React + React Flow** graph editor you can drive by hand *or* by natural language via a
> **grounded LLM companion**, over a **localhost FastAPI/WebSocket** server and one typed action
> space, and it **tunes its own parameters** to a target via the differentiable layer (R1 see it ·
> R2 edit it · R3 talk to it · R4 it tunes itself). Remaining: **R5** — agent **self-extension**
> (authoring new nodes). Plan: [`docs/pipeline/85`](docs/pipeline/85-phase2-agent-workbench-roadmap.md).
> **Last updated:** 2026-07-04.

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

See [`docs/theory/60-gaps-and-opportunities.md`](docs/theory/60-gaps-and-opportunities.md).

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
- Details: [`docs/theory/50-architecture-patterns.md`](docs/theory/50-architecture-patterns.md).

## Roadmap

High-level phases (synthesis of
[`docs/theory/60-*` build order](docs/theory/60-gaps-and-opportunities.md) and the
[I/O milestones](docs/pipeline/71-io-layer-milestones.md)). Checkboxes are the living
status — **kept current as we go**.

### Phase 0 — Foundations *(complete — foundation in place; M7/M9 non-gating)*
- [x] Deep-research dossier & design docs (`docs/`)
- [x] I/O layer foundation plan (`docs/pipeline/71-*`) & macOS capture plan (`docs/pipeline/70-*`)
- [x] **I/O layer** — duplex device capture+playback, full-duplex clock (M0–M4)
  *(M0 spikes; M1 core; M2 output; M3 input; M4 full-duplex — all ✅ merged)*
- [x] **Graph spine** — typed IR + eager executor + the node contract *(ADR-0009 + `docs/pipeline/74`; G1 IR · G2 executor · G3 live · G4 nodes+offline · G5 live edits · G6 Python bindings — all ✅)*
- [x] First end-to-end: capture → trivial graph (gain/meter) → playback, live *(G3 ✅ — graph driven by the Core Audio duplex backend)*
- [x] **Python control plane** — drive a *running* pipeline as a frontend: lock-free param command queue + atomic telemetry, and the RT output backend exposed control-only *(ADR-0010; G7 ✅ — Python never touches the audio thread)*
- [x] **Testing strategy** — documented C++ + Python layers (RT-safety/allocation, sanitizers, golden, cross-backend, live-device, packaging, notebooks) + one-command runner + CI *(`testing/README.md` ✅ CI-green)*

### Phase 1 — Differentiable core *(complete — D0–D8 landed)*
> **Implementation roadmap:** [`docs/pipeline/79-phase1-differentiable-core-roadmap.md`](docs/pipeline/79-phase1-differentiable-core-roadmap.md) — the third executor (Python/PyTorch, off-thread) over the same IR; milestones **D0–D8** (executor spine · linear nodes · trainable SVF filters · dynamics · losses+trainer · parameter-match slice · round-trip to RT · first neural node · DDSP exemplar), with the hard-problem mitigations, ADRs (0016/0017/0018), dependency graph, and definition of done.
- [x] Differentiable end-to-end graph execution — the optional `aiudio.diff` executor (PyTorch, off-thread; ADR-0016/0017) runs the same `Graph` IR through autograd with **C++↔torch parity**, auto-mirroring *any* graph (D0 + node-introspection enabler).
- [x] Classic DSP nodes as peers, **differentiable** — the full Tier-1 library (gain/mixer/pan, parametric EQ, waveshaper, DC blocker, compressor/gate, delay) has a differentiable form matching its C++ node (D1–D3); Tier 2/3 (spectral/convolution reverb) remains for later phases.
- [x] Losses + reproducible trainer — multi-res STFT loss + MSE/L1, `fit`/`seed_everything`/checkpoints (D4).
- [x] "Match a target" parameter optimization — `match_target` recovers a graph's params by backprop, then `export_to_graph` writes them into the C++ real-time graph (D5–D6; the gradient-tuning "brighten the vocal" slice — a *CLAP* objective is the Phase-2 upgrade).
- [x] First neural node (NAM-/RAVE-class in spirit) under the same contract — a torch `nn.Module` as a graph peer, trained jointly with DSP, exported via `torch.export`; RT inference is Phase 3 (D7).
- [x] DDSP exemplar — `HarmonicSynth` matches a target timbre via the STFT loss (D8).
- [x] Process-tap & offline/file backends (I/O M5–M6) *(M5 ✅ taps; M6 ✅ offline/file backend — both merged)*

### Phase 2 — Agent control plane & visual workbench *(the differentiator — 4 of 5 releases shipped)*
> **Plan:** [`docs/pipeline/85-phase2-agent-workbench-roadmap.md`](docs/pipeline/85-phase2-agent-workbench-roadmap.md) — a browser **visual graph editor** + a **grounded LLM companion** + **agent self-extension** (authoring new nodes), all driving one live engine through one typed action space, grounded in one capability manifest. Ships as five releases (R1 see it · R2 edit it · R3 talk to it · R4 it tunes itself · R5 it extends itself). Local desktop; React + React Flow; full-RT authored nodes behind a mandatory RT-safety gate; personal nodes in a local registry. **R5 detailed plan:** [`docs/pipeline/86`](docs/pipeline/86-r5-self-extension-plan.md).
- [x] Typed graph-edit action space (`add_node`/`connect`/`set_param`/…) — the shared substrate for UI, agent, and API + undo/replay log + capability manifest *(A0/A1 — `aiudio.workbench`)*
- [x] Visual workbench in the browser — see the full graph and **edit it by hand** (add/connect/tune/delete, undo, drag-layout, save/load) *(A2/B0–B2 — `aiudio.server` + `web/`)*
- [x] Grounded LLM companion — change the graph in natural language, grounded in the real capability manifest *(C0/C1 — `aiudio.agent`)*
- [x] LLM orchestrates structure; differentiable layer (`match_target`) tunes parameters *(C2 — `workbench.tune_to_target`)*
- [x] Render → measure → self-correct loop *(C2 engine; multi-res STFT — a CLAP perceptual metric + autonomous loop are follow-ups)*
- [ ] **R5** — agent self-extension: author new (full-RT, gated) nodes into a local personal registry, reusable thereafter *(D0–D3; plan in `docs/pipeline/86`)*

### Phase 3 — Real-time productionization
- [ ] C++ RT executor: inline + off-thread pooled neural nodes
- [ ] Streaming / cached-conv export (non-causal → causal)
- [ ] Plugin-host backend + VST3/CLAP packaging (I/O M7)
- [x] **True multi-source I/O** — N inputs + M outputs, one graph, from Python *(plan: [`docs/pipeline/76`](docs/pipeline/76-multi-source-io-roadmap.md) §11; **feature-complete — the full chain is merged**: G10 · G8 · G9 · M9.1 · M11a · M10 · master-clock adapter · M9.4 · M9.3 resampler (#21) · M9.5 off-clock drift servo (#22) · M9.6 cross-clock multi-device (#23) · M9.2 boundary channel mapping + real HAL device-died listener (#24) · **`LiveMultiSource`** N-source cross-clock engine (#30/#31) · **numpy WAV I/O** `WavReader`/`WavWriter` (#32) · **off-thread WAV recorder** `attach_wav_recorder` (#33) · signed **`aiudio-recorder.app`** — mic + system tap → mix → WAV, tap-testable without a loopback (#34). N sources → mix/route → M sinks via per-stream rings, on one or separate clocks, optionally recorded to a WAV. Only the physical hardware triggers (a real unplug; two devices on separate clocks) remain on-device, their logic proven headlessly via the mock)*

### Phase 4 — Breadth
- [ ] Source separation, more neural FX, neural-codec node
- [ ] Datasets + evaluation harness (SI-SDR, ViSQOL, FAD, MUSHRA/MOS)
- [ ] Offline generation path (diffusion/transformer)

> Milestone-level detail and acceptance criteria live in
> [`docs/pipeline/71-io-layer-milestones.md`](docs/pipeline/71-io-layer-milestones.md) and
> [`docs/theory/60-gaps-and-opportunities.md`](docs/theory/60-gaps-and-opportunities.md).

## Repository structure

```
aiudio/
├── README.md              ← this file (vision, roadmap, status)
├── CLAUDE.md              ← guidance for Claude Code: invariants, best practices, protocol
├── adr/                   ← Architecture Decision Records (why we chose what we chose)
│   ├── README.md          ← ADR process, lifecycle, index
│   ├── template.md        ← copy this for a new ADR
│   └── NNNN-*.md          ← one file per decision (append-only)
├── docs/                  ← documentation (index: docs/README.md)
│   ├── 00-vision-and-scope.md   · 90-references.md · _research-report.md
│   ├── theory/            ← research dossier (10–60) + fundamentals primers (73, 75)
│   ├── pipeline/          ← plans, milestones, capabilities, phase roadmaps (70–80, 85)
│   └── cookbooks/         ← runnable recipe guides (81–84)
├── examples/              ← cpp/ (C++ usage) + python/ (Python API) + M0 spikes
├── notebooks/             ← guided Jupyter tour of the Python-controllable pipeline
├── include/aiudio/io/     ← aiudio-io headers (devices, taps, duplex, WAV, offline)
├── include/aiudio/graph/  ← aiudio-graph headers (Node, Graph, executor, nodes)
├── src/io/, src/graph/    ← library implementations
├── bindings/              ← nanobind Python bindings (_aiudio module)
├── python/aiudio/         ← Python package (control frontend)
│   ├── diff/              ← optional differentiable layer (`aiudio[diff]`, PyTorch): executor,
│   │                        node registry, filters, losses, trainer, DDSP synth, parity harness
│   ├── workbench/         ← Phase 2 workbench substrate: typed graph-edit action space + log
│   │                        + graph↔JSON (ADR-0020) + capability manifest (ADR-0021)
│   ├── server/            ← Phase 2 localhost bridge (`aiudio[workbench]`): FastAPI + WebSocket
│   │                        serving the engine to the browser (ADR-0019); `python -m aiudio.server`
│   └── agent/             ← Phase 2 grounded LLM control plane (`aiudio[agent]`): Claude tool-use
│                            over the action space, manifest-grounded, consent-gated (ADR-0022)
├── web/                   ← Phase 2 visual workbench (React + React Flow, TS/Vite) — the browser
│                            graph editor over the WS; read-only in B0 (ADR-0019)
├── tests/                 ← C++ unit tests (CTest) + Python binding tests
├── testing/               ← test strategy (testing/README.md) + cross-cutting tests + run.sh
├── pyproject.toml         ← `pip install .` (scikit-build-core + nanobind)
└── CMakeLists.txt         ← C++ build (aiudio-io + aiudio-graph; -DAIUDIO_BUILD_PYTHON)
```

## Documentation

**New here / want to use it?** Start with the **[Pipeline Capabilities & Usage Guide
(`docs/pipeline/80`)](docs/pipeline/80-pipeline-capabilities.md)** — everything the pipeline does today (end of
Phase 0) with runnable **C++ and Python** examples.

For the design dossier, start at [`docs/README.md`](docs/README.md). It is grounded in a
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
| [0010](adr/0010-python-control-plane.md) | Python control plane — lock-free command queue, atomic telemetry, RT backend as control-only frontend |
| [0011](adr/0011-multi-stream-executor.md) | Multi-stream executor — N input/output streams via source/sink binding (extends ADR-0009) |
| [0012](adr/0012-per-port-channel-counts.md) | Per-port channel counts — channel-width propagation in the executor (extends ADR-0009) |
| [0013](adr/0013-latency-reporting-and-delay-compensation.md) | Latency reporting + graph-wide delay compensation (PDC) (extends ADR-0009) |
| [0014](adr/0014-multi-source-manager.md) | Multi-source manager — N sources + M sinks on one clock via per-stream rings (ADR-0008 §5) |
| [0015](adr/0015-boundary-resampling-and-cross-clock-drift.md) | Boundary sample-rate conversion + cross-clock drift compensation (M9.3/M9.5/M9.6) |
| [0016](adr/0016-differentiable-execution-strategy.md) | Differentiable execution — Python/PyTorch executor over the same IR (Phase 1 · D0) |
| [0017](adr/0017-autodiff-framework-pytorch.md) | Autodiff framework — PyTorch (optional `aiudio[diff]` extra) |
| [0018](adr/0018-trainable-filter-form.md) | Trainable-filter form — design-param + magnitude-response training, biquad coeff export (Phase 1 · D2) |
| [0019](adr/0019-visual-workbench-architecture.md) | Visual workbench — localhost server + React Flow browser control frontend (Phase 2) |
| [0020](adr/0020-graph-edit-action-space.md) | Graph-edit action space & protocol — one edit substrate (UI + agent + wire) + action log + graph↔JSON (Phase 2) |
| [0021](adr/0021-capability-manifest-grounding.md) | Capability manifest — registry-introspected node/param descriptors; the grounding source for UI + agent (Phase 2) |
| [0022](adr/0022-agent-runtime-and-consent-policy.md) | Agent runtime & human-in-the-loop — grounded tool-use; RT-invasive changes need active notify + confirm; structure-by-LLM/params-by-gradient (Phase 2) |
| [0023](adr/0023-personal-node-registry.md) | Personal node registry & isolation — local plugin dir, reusable, never auto-merged, promote→PR (Phase 2) |
| [0024](adr/0024-rt-safety-gate-and-plugin-abi.md) | RT-safety gate & node-plugin ABI — automatic pre-flight enforcing ADR-0004 on authored code; off-thread load (Phase 2) |

> **Significant decisions require an ADR.** See `CLAUDE.md` §9 for when to write
> one and how it ties into keeping the docs current.

## Getting started

**Build the C++ core + run tests** (macOS; `brew install cmake` if needed):

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

> **Full test suite, one command:** `testing/run.sh` (C++ build + ctest, sanitizers,
> `pip install`, ruff, pytest; `--live` adds the real-device layer). The complete
> testing strategy — C++ + Python, headless vs. live — is in
> [`testing/README.md`](testing/README.md).

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

**Use it from Python** (nanobind bindings — build a graph, run it on numpy):

```bash
pip install .          # scikit-build-core + nanobind
python examples/python/ex_graph_numpy.py
```
```python
import aiudio, numpy as np
g = aiudio.Graph()
src, gain, sink = g.add_source(), g.add_gain(0.5), g.add_sink()
g.connect(src, 0, gain, 0); g.connect(gain, 0, sink, 0)
ex = aiudio.GraphExecutor(); ex.compile(g, channels=1, sample_rate=48000, max_block=512)
out = ex.process(np.ones((1, 256), dtype=np.float32))   # -> numpy (1, 256), all 0.5
```

**Drive a live device as a control frontend** (macOS; ADR-0010) — Python opens/starts/stops
the stream, sends RT-safe parameter changes through a lock-free queue, and polls telemetry,
while the audio thread stays pure C++ (Python never touches it):

```bash
python examples/python/ex_live_control.py --list-devices
python examples/python/ex_live_control.py --seconds 3
```
```python
be = aiudio.DeviceBackend()
be.open(ex, channels=2, sample_rate=48000, block_size=512)  # GIL released
be.start()                                                  # C++ audio thread runs now
ex.set_gain(gain, 0.3)      # lock-free; applied at the next block — safe while running
ex.set_cutoff(lp, 1200.0)   # live filter change
blocks = ex.render_count    # telemetry: climbs while audio flows
be.stop()
```

**Take the full-feature tutorial** — [`notebooks/aiudio_feature_tutorial.ipynb`](notebooks/aiudio_feature_tutorial.ipynb)
is a **plotted, end-to-end teaching tour of all of Phase 0**: every mode of operation (numpy /
offline WAV / live device / headless mock / multi-source / cross-clock) and every feature (the
full node library with frequency-response & waveform plots, the live control plane, graph
editing, and the boundary DSP utilities). The shorter [`aiudio_pipeline_tour.ipynb`](notebooks/aiudio_pipeline_tour.ipynb)
is a gentler first pass; the terse feature+shortcomings checklist (and CI acceptance test) is
[`testing/notebooks/aiudio_acceptance_walkthrough.ipynb`](testing/notebooks/aiudio_acceptance_walkthrough.ipynb).
Prose reference: [`docs/pipeline/80`](docs/pipeline/80-pipeline-capabilities.md).

```bash
pip install . jupyter matplotlib
jupyter notebook notebooks/aiudio_feature_tutorial.ipynb
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
[`docs/pipeline/71-io-layer-milestones.md`](docs/pipeline/71-io-layer-milestones.md).

## Tech stack & key decisions

| Area | Choice | Rationale |
|---|---|---|
| RT core language | **C++20** (clang 21) | real-time safety, plugin/DSP/ML-runtime ecosystem |
| Research/ML language | **Python 3.11+** | PyTorch/JAX, agent, scripting |
| Interop | **nanobind** | low-overhead C++↔Python bindings |
| Differentiable layer | **PyTorch** (optional `aiudio[diff]`) | autograd over the same IR; trainable DSP + neural (ADR-0016/0017) |
| Workbench (Phase 2) | **FastAPI + WebSocket** server (optional `aiudio[workbench]`) · **React + React Flow** browser UI | visual graph editor + agent companion over the action space (ADR-0019/0020) |
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
distinct licensing implications; see `docs/theory/30-*` §7).

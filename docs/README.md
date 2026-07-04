# aiudio — Documentation

Research, vision, and design docs for **aiudio**, an AI-native digital audio
processing framework. Organized into three folders:

- **[`theory/`](theory/)** — the *why/what*: the SOTA research dossier + audio fundamentals.
- **[`pipeline/`](pipeline/)** — the *how*: implementation plans, milestones, capabilities, phase roadmaps.
- **[`cookbooks/`](cookbooks/)** — runnable recipe guides for using the pipeline.

## Start here

1. **[00-vision-and-scope.md](00-vision-and-scope.md)** — what aiudio is, the
   locked scoping decisions, non-goals, and the hard problems.

## Theory & research — [`theory/`](theory/)

The SOTA dossier is grounded in a deep, multi-source, fact-checked research pass (each claim of
consequence is cited; the consolidated source list is [`90-references.md`](90-references.md)), plus
two fundamentals primers.

| Doc | Contents |
|---|---|
| [theory/10-landscape-and-frameworks.md](theory/10-landscape-and-frameworks.md) | Existing audio frameworks/libraries (torchaudio, JUCE, Faust, Max/MSP, RTNeural, Neutone, nn~, Web Audio, Rust audio…) and the signal-graph/dataflow patterns they use. |
| [theory/20-differentiable-dsp-and-neural-audio.md](theory/20-differentiable-dsp-and-neural-audio.md) | DDSP, RAVE, neural vocoders, neural codecs (EnCodec/SoundStream/DAC), source separation (Demucs/Spleeter/Open-Unmix), neural FX/amp modeling, diffusion/transformer generation (MusicGen/Stable Audio). |
| [theory/30-realtime-neural-inference.md](theory/30-realtime-neural-inference.md) | Real-time/streaming neural inference, latency budgets, causal models, ONNX/LibTorch/TorchScript, quantization, neural-plugin landscape (Neutone, NAM), VST/AU/CLAP. |
| [theory/40-ai-agents-for-audio.md](theory/40-ai-agents-for-audio.md) | LLM agents for audio/music: natural-language-to-DSP, copilots, tool-use orchestration, symbolic/MIDI agents — what's been tried and what works. |
| [theory/50-architecture-patterns.md](theory/50-architecture-patterns.md) | Signal/dataflow graph engine design, eager vs lazy, Python+C++ interop (pybind11/nanobind), differentiable rendering, real-time-safe audio threading. |
| [theory/60-gaps-and-opportunities.md](theory/60-gaps-and-opportunities.md) | What does NOT exist yet; where an AI-native framework can uniquely win; design risks and open problems. |
| [theory/73-digital-audio-encoding.md](theory/73-digital-audio-encoding.md) | **Primer:** how audio is encoded digitally — sampling/Nyquist, quantization/bit depth, integer vs. float PCM, dBFS, channels/frames, interleaved vs. planar, blocks/latency, data rates, beyond-PCM codecs — each tied to where the pipeline implements it. |
| [theory/75-how-adcs-and-dacs-work.md](theory/75-how-adcs-and-dacs-work.md) | **Primer:** how the **analog↔digital converters** work (the black box `73` treats as external) — the ADC/DAC chains, sigma-delta/oversampling, resolution/ENOB/dither, and the two properties that reach into the pipeline: the converter **sample clock** (= the master clock, ADR-0005; drift → the `ResamplingSource` servo) and **conversion latency** (feeds PDC). |
| [90-references.md](90-references.md) | Consolidated, deduplicated, cited reference list (papers, projects, standards, datasets). *(at docs root)* |
| [_research-report.md](_research-report.md) | Raw synthesized deep-research output, kept for provenance. *(at docs root)* |

## Pipeline — [`pipeline/`](pipeline/)

Implementation plans, milestones, capabilities, and phase roadmaps — how the engine is actually built.

| Doc | Contents |
|---|---|
| [pipeline/70-macos-audio-capture-plan.md](pipeline/70-macos-audio-capture-plan.md) | Concrete plan to tap local macOS audio sources (input devices, system output, per-app audio) and feed them into the graph engine — API decision matrix, recommended C++/Python architecture, milestones, permissions/packaging gotchas, runnable spike. |
| [pipeline/71-io-layer-milestones.md](pipeline/71-io-layer-milestones.md) | **Foundation milestone plan** for the full I/O layer (input **and** output) — the unifying duplex-callback/swappable-clock model, core C++ abstractions, M0–M9 milestones with acceptance criteria, dependency graph, and definition of done. |
| [pipeline/72-m1-aiudio-io-reference.md](pipeline/72-m1-aiudio-io-reference.md) | **M1 reference** — code-grounded description of exactly what the `aiudio-io` library implements: the value types, `RenderCallback`/`AudioBackend` contracts, the lock-free SPSC ring buffer, conversions, build, tests, examples, RT-safety summary, and what M1 leaves to M2+. |
| [pipeline/74-graph-spine-milestones.md](pipeline/74-graph-spine-milestones.md) | **Graph-spine milestone plan** (ADR-0009) — the typed IR + node contract + eager executor that turns the I/O backends into a pipeline: core abstractions, milestones G1–G6 (toward live `capture → graph → playback`), dependency graph, definition of done, and what the spine defers to Phase 1/2. |
| [pipeline/76-multi-source-io-roadmap.md](pipeline/76-multi-source-io-roadmap.md) | **True multi-source I/O project plan** (absorbs the M9 hardening plan as its Phase B) — N inputs + M outputs on one clock, one graph, from Python, RT-safe. The three-layer model (alignment · composition · mixing/routing), engine prerequisites (G8/G9), phases A–E, master dependency graph, new ADRs, definition of done, risks. |
| [pipeline/77-combining-multiple-audio-io.md](pipeline/77-combining-multiple-audio-io.md) | **Explainer:** *why* combining multiple audio inputs/outputs in one pipeline is hard — the independent hazards (RT deadline, many clocks & drift, rate/format/channel/block heterogeneity, latency alignment/PDC, lock-free hand-off, routing/mixing, hot-plug, xrun semantics), how they compound, and the principles (one clock per source, align-then-combine, mixing-as-nodes, off-thread lifecycle) that tame them. Companion to `76`. |
| [pipeline/78-node-library-roadmap.md](pipeline/78-node-library-roadmap.md) | **Node-library roadmap** (DSP + neural peers) — growing the graph palette in three tiers. **Tier 1 ✅** (parametric-EQ family, compressor/gate, delay, waveshaper, oscillator/noise, pan/width, mixer, channel-matrix, DC blocker); **Tier 3 ◑** (differentiable + a first neural node landed in Phase 1); Tier 2 (spectral/convolution reverb, loudness meters) planned — with per-node contract properties + the enablers each needs. |
| [pipeline/79-phase1-differentiable-core-roadmap.md](pipeline/79-phase1-differentiable-core-roadmap.md) | **Phase 1 roadmap — the differentiable core (✅ COMPLETE, D0–D8).** The *third executor* (Python/PyTorch, off-thread) that runs the same `Graph` IR through autograd so DSP params are trainable and neural models are peers; proven by parameter optimization and closed by exporting trained params back to the C++ RT graph. Foundation abstractions, the hard problems + mitigations, milestones D0–D8, dependency graph, ADRs (0016/0017/0018), definition of done. |
| [pipeline/80-pipeline-capabilities.md](pipeline/80-pipeline-capabilities.md) | **Pipeline capabilities & usage guide (end of Phase 0)** — everything the pipeline can do today, with runnable **C++ and Python**: core concepts, the full node-library catalog, 1-stream/multi-stream graphs, the live control plane, graph editing/introspection, the offline/live-device/mock backends, multi-source + cross-clock, the boundary DSP utilities, end-to-end recipes, and an honest list of what Phase 0 does *not* do yet. The companion the cookbooks branch from. |
| [pipeline/85-phase2-agent-workbench-roadmap.md](pipeline/85-phase2-agent-workbench-roadmap.md) | **Phase 2 roadmap — Agent Control Plane & Visual Workbench.** A browser **visual graph editor** + a **grounded LLM companion** + **agent self-extension** (authoring new nodes on the fly), all driving one live engine through one typed action space, grounded in one capability manifest. Architecture, the hard problems (chiefly RT-safety of agent-authored C++ + its gate), milestones across four workstreams, a five-release shipping ladder (R1 see it → R5 it extends itself), ADRs 0019–0024, definition of done. **R1–R4 shipped; R5 remaining.** |
| [pipeline/86-r5-self-extension-plan.md](pipeline/86-r5-self-extension-plan.md) | **R5 implementation plan — agent self-extension (D0–D3).** The detailed plan for the agent authoring new node *types* at runtime: the node-plugin C ABI + a runtime node registry, the local personal package registry (ADR-0023), the spec→C++ scaffold + off-thread build, the **RT-safety pre-flight gate** (static + RTSan/alloc-hook + contract → pass/quarantine/discard, ADR-0024), and the agent authoring loop. Architecture + diagram, hard-problems/feasibility table, milestones as sub-PRs, the RT-gate + plugin-ABI designs, the security model, a headless testing strategy, and honest PoC caveats. |

## Cookbooks — [`cookbooks/`](cookbooks/)

Runnable recipe guides — *how to use* the pipeline. Companions to `pipeline/80` (capabilities).

| Doc | Contents |
|---|---|
| [cookbooks/81-pipeline-usage-patterns.md](cookbooks/81-pipeline-usage-patterns.md) | **Usage-patterns cookbook** — the canonical topologies along each axis (**offline↔live**, **one↔many sources**, **single↔multiple clocks**, **live↔recorded output**), each answered against a 6-question checklist, with **complete C++ and Python**: offline render, offline multi-source mix, live duplex monitor, single-clock multi-source (`MultiSourceManager`), separate-clock multi-source (`LiveMultiSource`), recorded output, and the input-clock no-playback recorder engine. The "how to assemble a topology" doc. |
| [cookbooks/82-node-usage-patterns.md](cookbooks/82-node-usage-patterns.md) | **Node-usage cookbook** — how to *use the nodes*: typical DSP chains and idioms, with runnable **C++ and Python**. Node fundamentals (ports/fan-out, generators/processors/sinks, live params, channel-width changes, PDC, metering), then 13 patterns — gain staging, biquad tone shaping, the parametric EQ, compression/limiting, gating, saturation + DC blocker, a synth voice, delay (insert vs send), Sum-vs-Mixer, parallel compression, stereo pan/width, the channel matrix, and a channel-strip capstone — plus a per-node parameter-index quick reference. |
| [cookbooks/83-live-control-and-dynamic-graphs.md](cookbooks/83-live-control-and-dynamic-graphs.md) | **Live-control cookbook** — how to *change a running graph* (ADR-0010), with runnable **C++ and Python**. The two-rule model (**values** → the lock-free `set_param` queue, click-free; **structure** → edit + `compile()` → atomic RCU swap), then patterns: change a param live, control-rate automation + smoothing, a name→`(node, param)` control surface, inserting/bypassing/removing nodes live, reading the graph back, and closing the loop (meter→param auto-gain). Appendices: what's RT-safe from the control thread, the atomic-swap/lifetime rules. |
| [cookbooks/84-differentiable-and-trainable-graphs.md](cookbooks/84-differentiable-and-trainable-graphs.md) | **Differentiable & trainable graphs cookbook** — how to *train* a graph via the optional `aiudio.diff` layer (Phase 1 · D0–D8, PyTorch). The third-executor model + parity harness, making a graph differentiable (`DiffExecutor`), how autograd computes gradients, losses (multi-res STFT + MSE/L1), the `fit`/checkpoint harness, matching a target render (`match_target`), the round-trip to real time (`export_to_graph`), a neural node as a peer (`NeuralNode` + `torch.export`), and a DDSP `HarmonicSynth` exemplar. The **ML-first** cookbook (ADR-0016/0017/0018). |

## How this dossier was produced

The research docs are synthesized from a deep-research pass (fan-out web search →
source fetch → adversarial claim verification → cited synthesis). Findings are
labelled by confidence where the underlying verification was mixed. Treat
anything marked *low confidence* or *unverified* as a lead to confirm, not a
settled fact.

---

*Status: Phase 0 + Phase 1 complete; Phase 2 planned. Docs organized into theory / pipeline / cookbooks.*

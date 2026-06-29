# aiudio — Documentation

Research, vision, and design docs for **aiudio**, an AI-native digital audio
processing framework.

## Start here

1. **[00-vision-and-scope.md](./00-vision-and-scope.md)** — what aiudio is, the
   locked scoping decisions, non-goals, and the hard problems.

## Research dossier (state of the art)

These are grounded in a deep, multi-source, fact-checked research pass. Each
claim of consequence is cited; the consolidated source list lives in
`90-references.md`.

| Doc | Contents |
|---|---|
| [10-landscape-and-frameworks.md](./10-landscape-and-frameworks.md) | Existing audio frameworks/libraries (torchaudio, JUCE, Faust, Max/MSP, RTNeural, Neutone, nn~, Web Audio, Rust audio…) and the signal-graph/dataflow patterns they use. |
| [20-differentiable-dsp-and-neural-audio.md](./20-differentiable-dsp-and-neural-audio.md) | DDSP, RAVE, neural vocoders, neural codecs (EnCodec/SoundStream/DAC), source separation (Demucs/Spleeter/Open-Unmix), neural FX/amp modeling, diffusion/transformer generation (MusicGen/Stable Audio). |
| [30-realtime-neural-inference.md](./30-realtime-neural-inference.md) | Real-time/streaming neural inference, latency budgets, causal models, ONNX/LibTorch/TorchScript, quantization, neural-plugin landscape (Neutone, NAM), VST/AU/CLAP. |
| [40-ai-agents-for-audio.md](./40-ai-agents-for-audio.md) | LLM agents for audio/music: natural-language-to-DSP, copilots, tool-use orchestration, symbolic/MIDI agents — what's been tried and what works. |
| [50-architecture-patterns.md](./50-architecture-patterns.md) | Signal/dataflow graph engine design, eager vs lazy, Python+C++ interop (pybind11/nanobind), differentiable rendering, real-time-safe audio threading. |
| [60-gaps-and-opportunities.md](./60-gaps-and-opportunities.md) | What does NOT exist yet; where an AI-native framework can uniquely win; design risks and open problems. |
| [90-references.md](./90-references.md) | Consolidated, deduplicated, cited reference list (papers, projects, standards, datasets). |
| [_research-report.md](./_research-report.md) | Raw synthesized deep-research output, kept for provenance. |

## Implementation plans

| Doc | Contents |
|---|---|
| [70-macos-audio-capture-plan.md](./70-macos-audio-capture-plan.md) | Concrete plan to tap local macOS audio sources (input devices, system output, per-app audio) and feed them into the graph engine — API decision matrix, recommended C++/Python architecture, milestones, permissions/packaging gotchas, runnable spike. |
| [71-io-layer-milestones.md](./71-io-layer-milestones.md) | **Foundation milestone plan** for the full I/O layer (input **and** output) — the unifying duplex-callback/swappable-clock model, core C++ abstractions, M0–M9 milestones with acceptance criteria, dependency graph, and definition of done. |
| [72-m1-aiudio-io-reference.md](./72-m1-aiudio-io-reference.md) | **M1 reference** — code-grounded description of exactly what the `aiudio-io` library implements: the value types, `RenderCallback`/`AudioBackend` contracts, the lock-free SPSC ring buffer, conversions, build, tests, examples, RT-safety summary, and what M1 deliberately leaves to M2+. |
| [73-digital-audio-encoding.md](./73-digital-audio-encoding.md) | **Primer:** how audio is encoded in digital systems — sampling/Nyquist, quantization/bit depth, integer vs. float PCM, dBFS, channels/frames, interleaved vs. planar, blocks/latency, data rates, and beyond-PCM codecs — each tied to where aiudio's pipeline implements it. |
| [74-graph-spine-milestones.md](./74-graph-spine-milestones.md) | **Graph-spine milestone plan** (ADR-0009) — the typed IR + node contract + eager executor that turns the I/O backends into a pipeline: core abstractions, milestones G1–G6 (toward live `capture → graph → playback`), dependency graph, definition of done, and what the spine defers to Phase 1/2. |

## How this dossier was produced

The research docs are synthesized from a deep-research pass (fan-out web search →
source fetch → adversarial claim verification → cited synthesis). Findings are
labelled by confidence where the underlying verification was mixed. Treat
anything marked *low confidence* or *unverified* as a lead to confirm, not a
settled fact.

---

*Status: scaffold complete; research docs populated from the deep-research run.*

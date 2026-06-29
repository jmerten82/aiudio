# 30 — Real-Time Neural Inference & the Plugin Landscape

How neural models actually run inside a real-time audio engine (the framework's
hardest constraint), and the existing tools/standards aiudio should build on or
interoperate with.

> **Provenance key:** **✓ Verified** = cited from the deep-research pass.
> **○ Background** = established knowledge, confirm before relying.

---

## 1. The core constraint: real-time safety on the audio thread ✓

A real-time audio callback runs on a high-priority thread that must **never
block**: no memory allocation, no locks, no syscalls, no unbounded work. At
48 kHz with a 128-sample buffer you have **~2.7 ms** to produce each block;
miss it and you get an audible glitch (xrun).

This single rule explains the entire neural-audio-inference tooling landscape:

> **✓ Mainstream PyTorch / TensorFlow are unsafe on the audio thread.** They rely
> on dynamically resizable structures (e.g. C++ `std::vector`) that **allocate
> memory during inference**. Two strategies have emerged to fix this:
> 1. **Make the inference engine itself real-time-safe** (pre-allocate
>    everything) → *RTNeural*.
> 2. **Keep the heavy engine, but move inference OFF the audio thread** onto a
>    pre-allocated worker pool → *ANIRA*.

---

## 2. RTNeural — real-time-safe inference engine ✓

[github.com/jatinchowdhury18/RTNeural](https://github.com/jatinchowdhury18/RTNeural)
· paper [arXiv:2106.03037](https://arxiv.org/pdf/2106.03037) · Jatin Chowdhury
(CCRMA).

- **Lightweight, header-capable C++**, "designed with the intention of being used
  in real-time systems, specifically real-time audio processing."
- **Key axiom:** *"RTNeural allocates all required memory while loading the model
  so that memory is never allocated as part of the real-time process."*
- **Backends:** Eigen, XSIMD, C++ STL (+ experimental Apple Accelerate).
- **APIs:** run-time *and* compile-time. The compile-time API is fastest (the
  model topology is known to the compiler → loop unrolling).
- **Layers:** Dense, GRU, LSTM, Conv1D/2D, MaxPooling, BatchNorm + activations.
  *(This exact layer list was the one sub-claim that verified 2-1, not 3-0 —
  treat the precise set as version-dependent.)*
- **Benchmarks (48 kHz, Intel i7-9750H):** RTNeural beats the PyTorch C++ API for
  **smaller** layer sizes (compile-time API fastest); PyTorch scales better to
  **large** layers. ⚠️ *Tied to 2019 hardware / 2021 library versions.*
- **In the wild:** it is the inference engine behind **NAM (Neural Amp Modeler)**
  and **GuitarML** — the canonical real-time neural-effect success story.

**Fit for aiudio:** the right engine for **small, causal, per-sample/per-block
models as real-time DSP nodes** (amp sims, small denoisers, control models). C++
core, header-only embeddable.

---

## 3. ANIRA — decouple inference from the audio callback ✓

[github.com/anira-project/anira](https://github.com/anira-project/anira) · paper
[arXiv:2506.12665](https://arxiv.org/abs/2506.12665) (TU Berlin, IEEE IS² 2024).

- "A high-performance library designed to enable easy **real-time-safe
  integration of neural network inference** within audio applications."
- **One abstraction over three runtimes**, selectable at runtime:
  **LibTorch · ONNX Runtime · TensorFlow Lite (LiteRT)**.
- **Real-time safety strategy:** *"decoupling the inference from the audio
  callback to a **static thread pool**"* — pre-allocated, avoids oversubscription,
  deterministic runtimes. **Inference runs OFF the audio thread.**
- **Trade-off (○):** moving inference off-thread adds **latency** (the result
  comes back a block or more later). ANIRA manages this with its threading model;
  the design tension is *throughput/quality (big model, off-thread)* vs
  *latency (small model, on-thread)*.

**Fit for aiudio:** the right pattern for **larger neural nodes** (RAVE-class
synths, separators, codecs) that can't run inline on the audio thread. ANIRA is
also a strong reference architecture (and possibly a direct dependency) for
aiudio's real-time core.

> **✓ Architectural lesson:** *one real-time audio inference layer can wrap
> multiple ML backends.* aiudio should not bind to a single runtime — abstract
> over LibTorch/ONNX/TFLite (and future runtimes) behind a node interface, as
> ANIRA proves is feasible.

---

## 4. Streaming / causal models — making non-causal nets real-time ✓

The model side of the latency problem (complementary to the engine side).

- **Cached convolutions** (RAVE / [cached_conv](https://acids-ircam.github.io/cached_conv/)):
  models **trained non-causally** can be **converted to streaming** after
  training via post-hoc reconfiguration, *avoiding* the quality loss of
  specialized causal training. (✓)
- **Streamable codecs:** SoundStream / Mimi are **streamable and real-time on a
  smartphone CPU** by construction (see `20-*` §4). (✓)
- **BRAVE** ([arXiv:2503.11562](https://arxiv.org/html/2503.11562v2)): **<10 ms**
  total latency neural synthesis. (✓)
- **LLVC** ([arXiv:2311.00873](https://arxiv.org/pdf/2311.00873)): low-latency
  **voice conversion under 20 ms** at 16 kHz — neural VC can meet the framework's
  budget. (✓)

**Takeaway:** the <10–20 ms budget is *achievable* for purpose-built streaming
models, but it is **not free** — it constrains architecture (causality, receptive
field, model size). aiudio should make "real-time-capable?" an explicit,
enforced property of each node, with the streaming-export path as a first-class
workflow.

---

## 5. Inference runtimes & model formats ○ (background)

> Background consolidation; ANIRA's three-backend choice (✓) is the verified anchor.

| Runtime | Niche | Notes |
|---|---|---|
| **LibTorch** (PyTorch C++) | Research→deploy continuity | Heaviest; TorchScript / `torch.export` for serialization; not RT-safe inline |
| **ONNX Runtime** | Portable cross-framework | Broad operator support, graph optimizations, EPs (CoreML, CUDA, etc.) |
| **TensorFlow Lite / LiteRT** | Mobile/embedded | Smallest footprint; strong quantization story |
| **RTNeural** | Hard-RT small models | Pre-allocated, header-only (see §2) |
| **TorchScript / torch.export** | Serialization | The model-handoff format used by nn~/RAVE |

**Quantization** (○): INT8 / dynamic / QAT shrinks models and speeds inference —
important for embedded and for fitting the latency budget; pairs naturally with
TFLite/ONNX. A relevant aiudio concern but **not** covered by the verified pass.

---

## 6. The neural-plugin deployment landscape ✓/○

How neural models reach a DAW today — the glue aiudio must provide or interoperate
with.

- **Neutone SDK** ✓ ([arXiv:2508.09126](https://arxiv.org/abs/2508.09126),
  [repo](https://github.com/Neutone/neutone_sdk)) — the **PyTorch→DAW deployment
  layer**. Researchers stay in **Python**; a **model-agnostic** interface
  streamlines deployment for **both real-time and offline** use, encapsulating
  **variable buffer sizes, sample-rate conversion, delay compensation, control
  parameter handling**. Ships free host plugins **Neutone FX** (real-time) and
  **Neutone Gen** (offline). It explicitly names aiudio's target gap:
  *"integrating deep learning models into DAWs remains challenging due to
  real-time / neural network inference constraints and the complexities of plugin
  development."*
- **nn~ (nn_tilde)** ✓ ([repo](https://github.com/acids-ircam/nn_tilde)) — IRCAM's
  "translation layer between Max/MSP or PureData and the libtorch C++ interface";
  how RAVE runs live in Max/PD.
- **NAM / GuitarML** ○ — domain-specific neural-amp plugins built on RTNeural;
  proof that small neural effects ship as ordinary VST/AU today.

> **Design implication:** Neutone is both a **reference design** for the
> Python-authoring / C++-hosting split aiudio wants, and a potential **interop
> target** (export aiudio graphs as Neutone models, or host Neutone models as
> aiudio nodes).

---

## 7. Plugin standards ○ (background — not in verified pass)

aiudio is a *framework/engine*; to be useful in music production it must speak the
host plugin formats.

| Format | Owner | Notes |
|---|---|---|
| **VST3** | Steinberg | Dominant cross-platform standard; GPLv3-or-proprietary licensing |
| **AU (Audio Unit)** | Apple | Required for Logic / macOS/iOS hosts |
| **AAX** | Avid | Pro Tools only |
| **CLAP** | Bitwig + u-he (open) | Modern, liberally-licensed (MIT), good threading & per-note expression; rising fast |
| **LV2** | open (Linux) | Linux/embedded ecosystem |

- **JUCE** (○) is the de-facto C++ framework for *authoring* plugins across
  VST3/AU/AAX/standalone from one codebase — the likely host-integration layer
  for aiudio's C++ core. **CLAP** is the strategically interesting open target
  (clean threading model, no licensing friction).

---

## 8. What this means for aiudio

1. **Two real-time execution modes, both validated** (✓): inline RT-safe small
   models (RTNeural pattern) **and** off-thread pooled inference for big models
   (ANIRA pattern). aiudio's scheduler needs both, chosen per node.
2. **Abstract over inference runtimes** (✓ via ANIRA) — don't marry one.
3. **Make "real-time-capable" a first-class, enforced node property** (✓ via
   streaming-model evidence) with a streaming/cached-conv export path.
4. **Adopt the Python-author / C++-host split** (✓ via Neutone) and interoperate
   with Neutone, nn~, VST3/AU/CLAP rather than reinventing host glue.
5. **Quantization, plugin packaging, and CLAP support** are real line-items but
   currently **unverified background** (○) — confirm in a follow-up pass.

See `50-architecture-patterns.md` for the engine/threading design that ties this
together.

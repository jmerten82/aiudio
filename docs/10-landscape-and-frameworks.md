# 10 — Landscape & Frameworks

Existing audio frameworks/libraries, and the **signal-graph / dataflow patterns**
they use — the prior art aiudio builds on, borrows from, and interoperates with.

> **Provenance key:** **✓ Verified** = cited from the deep-research pass.
> **○ Background** = established knowledge, confirm before relying.
>
> The verified pass concentrated on the *neural-inference / deployment* tools
> (RTNeural, ANIRA, Neutone, nn~ — all ✓). The classic DSP/computer-music
> frameworks below are **○ background**: widely-known but not re-verified here.

---

## 1. The landscape in one table

| Tool | Language | Primary role | Graph/dataflow model | aiudio relevance |
|---|---|---|---|---|
| **torchaudio** | Python/C++ | ML audio I/O, transforms, datasets | Eager tensor ops (PyTorch) | ML-layer dependency (○) |
| **librosa** | Python | Analysis/feature extraction | Eager, NumPy arrays, offline | Feature/analysis tools (○) |
| **DDSP (library)** | Python (TF/JAX) | Differentiable synths/effects | Differentiable eager graph | Core pattern source ✓ (`20-*`) |
| **Essentia** | C++/Python | MIR feature extraction | Streaming + standard "algorithm" graph | Analysis tools (○) |
| **JUCE** | C++ | Plugin/app framework | Pull-based audio callback; `AudioProcessorGraph` | Likely host-integration layer (○) |
| **RTNeural** | C++ | RT-safe NN inference | Pre-allocated layer chain | RT neural nodes ✓ (`30-*`) |
| **ANIRA** | C++ | RT-safe NN inference (multi-backend) | Off-thread static pool | RT core reference ✓ (`30-*`) |
| **Neutone SDK** | Python→C++ | PyTorch→DAW deployment | Model-agnostic wrapper | Deploy/interop ✓ (`30-*`) |
| **nn~ (nn_tilde)** | C++ | Torch models in Max/PD | Max/PD object in a patch graph | Interop target ✓ (`30-*`) |
| **Faust** | Faust DSL→C++/etc. | DSP language/compiler | **Functional block diagram algebra** | Strong design reference (○) |
| **Max/MSP** | C (host) | Visual programming | **Patcher: explicit dataflow graph** | UX/graph reference (○) |
| **Pure Data (Pd)** | C | Visual programming (open) | Patcher dataflow graph | UX/graph reference (○) |
| **SuperCollider** | C++/sclang | Synthesis server + language | **Server-side UGen graph (SynthDef)** | Graph/scheduling reference (○) |
| **Web Audio API** | JS/C++ | Browser audio | **AudioNode graph (pull)** | Graph-model reference (○) |
| **Rust audio** | Rust | Emerging RT ecosystem | Various (see §5) | Alt. core language (○) |

---

## 2. ML / analysis layer (Python) ○

- **torchaudio** — PyTorch's audio I/O, transforms (spectrograms, resampling),
  datasets, and some models. Eager tensor dataflow. The natural substrate for
  aiudio's Python ML layer.
- **librosa** — the standard Python analysis/feature library (STFT, mel, chroma,
  beat, onset). Offline, NumPy-based. Good for the agent's "ears" (analysis
  tools, `40-*` §4).
- **Essentia** (MTG-UPF) — C++ with Python bindings; large MIR algorithm
  collection; supports a **streaming mode** with a connected graph of algorithms.
  A useful reference for "analysis as a dataflow graph."
- **DDSP library** (Magenta) — the reference implementation of the differentiable
  synths/effects in `20-*`. ✓ pattern source.

---

## 3. Real-time neural inference / deployment ✓

Covered in depth in `30-realtime-neural-inference.md`. Summary of the
**graph/threading patterns** (the part relevant here):
- **RTNeural** ✓ — a **pre-allocated layer chain**; all memory bound at load,
  nothing allocated during inference. Pattern: *the model IS the real-time graph*.
- **ANIRA** ✓ — a **static thread pool** decoupling inference from the audio
  callback; one abstraction over LibTorch/ONNX/TFLite. Pattern: *the graph spans
  two thread domains (RT callback ↔ worker pool)*.
- **Neutone SDK** ✓ — a **model-agnostic wrapper** that hides buffer-size /
  sample-rate / delay-compensation plumbing. Pattern: *the node presents a clean
  block-processing contract regardless of the model inside*.
- **nn~** ✓ — exposes a Torch model as **one object in a Max/PD patcher graph**.
  Pattern: *neural model as a peer node in an existing dataflow host* — exactly
  aiudio's pillar-1 goal, in miniature.

---

## 4. Classic DSP & computer-music frameworks — graph models to learn from ○

> These are the richest sources of **signal-graph design wisdom**. Background, not
> re-verified.

- **Faust** — a **functional DSP language**: programs are *block diagrams* built
  from a small algebra of composition operators (sequential `:`, parallel `,`,
  recursive `~`, split `<:`, merge `:>`). Compiles to highly optimized C++, LLVM,
  WASM, etc. **Why it matters:** the cleanest existing model of "a DSP graph as a
  composable, compilable algebra" — a strong reference for aiudio's IR (`50-*`).
- **Max/MSP** (Cycling '74) & **Pure Data** (Miller Puckette, open) — **visual
  patcher** environments: the user wires an explicit dataflow graph of objects;
  audio (`~`) signals flow at sample/block rate. **Why it matters:** the dominant
  *UX* for audio graphs, and the host where nn~/RAVE already run. aiudio's agent
  edits a graph of the same conceptual shape.
- **SuperCollider** — a real-time **synthesis server (scsynth)** driven by a
  language (sclang); sound is a **UGen graph** compiled into a **SynthDef** and
  scheduled on the server. **Why it matters:** clean **client/server +
  graph-as-compiled-artifact** separation, and a mature real-time scheduling
  model.
- **Web Audio API** — browser standard; an **AudioNode graph** with **pull-based**
  rendering (the destination pulls from its inputs). **Why it matters:** the most
  widely-deployed audio-graph API; a reference for node/param automation
  semantics; a possible WASM deployment target.
- **CSound / CLAP-adjacent ecosystems** — older but instructive on score/orchestra
  separation and real-time scheduling.

**Common thread:** every successful audio system is, underneath, a **dataflow
graph of unit generators** with a **pull or push** evaluation model and a
**block/sample** execution granularity. aiudio's contribution is to make
**neural nodes and an agent first-class** in that long-established structure —
not to invent the structure.

---

## 5. Rust audio ecosystem ○ (background)

Relevant because Rust offers RT-friendly performance + memory safety + a strong
WASM story (an alternative or complement to the C++ core):
- **CPAL** (cross-platform audio I/O), **FunDSP** (functional, Faust-like graph
  DSL in Rust), **dasp** (DSP primitives), **nih-plug** (modern plugin framework,
  CLAP/VST3), **Glicol** (graph-oriented live-coding language).
- **Trade-off vs the chosen C++ core:** Rust buys safety and WASM; C++ buys the
  mature plugin/DSP/ML-runtime ecosystem (JUCE, LibTorch, RTNeural, ANIRA all
  C++). The locked decision (`00-*`) is **C++ core + Python ML**; Rust is a
  noted alternative, not the path.

---

## 6. Where aiudio sits

aiudio is **not** another entry in any single column above — it deliberately
spans them:
- It needs a **Faust/Max-like composable graph** (classic-DSP wisdom, ○),
- with **RTNeural/ANIRA-grade real-time neural nodes** (✓),
- a **torchaudio/DDSP-grade differentiable ML layer** (✓ pattern),
- a **Neutone/nn~-grade deployment + host interop story** (✓),
- and an **agent control plane on top** that none of them have (✓ gap, `40-*`).

No existing framework unifies classic DSP, neural models, differentiability,
real-time+offline, *and* an agent in one graph — that unification is the thesis
(`60-gaps-and-opportunities.md`).

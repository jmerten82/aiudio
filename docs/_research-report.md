# Deep-Research Report (Provenance)

> This is the raw synthesized output of the deep-research pass that grounds the
> rest of this dossier. It is kept verbatim-in-substance for provenance. The
> topic docs (`10-*`–`60-*`) reorganize and extend this material; where they add
> facts **not** in this verified set, they say so explicitly.

**Method:** 5 search angles → 18 sources fetched → 89 claims extracted → top 25
claims adversarially verified (3-vote, need 2/3 refutes to kill) → 8 synthesized
findings. **25/25 claims confirmed, 0 killed.** 100 agent calls.

**Date of run:** 2026-06-28.

---

## Executive summary

The state of the art already supplies **most of the building blocks** for the
proposed AI-native audio framework, but **no single system unifies them** as the
vision describes.

1. **Differentiable DSP** (DDSP, ICLR 2020, + its Frontiers/2023 review) is the
   proven mechanism for making classic DSP blocks and neural models composable,
   jointly-trainable peers — gradients backpropagate through signal processors —
   and **MIDI-DDSP** shows hierarchical, disentangled, editable control on top.
2. **Real-time neural inference** is solved in production by purpose-built C++
   engines (**RTNeural**'s no-allocation-on-audio-thread design; **ANIRA**'s
   static thread pool decoupling inference from the audio callback; with
   LibTorch / ONNX Runtime / TFLite backends) and by **streaming-causal model
   designs** (RAVE's cached convolutions; SoundStream/Mimi streamable codecs
   running real-time on a smartphone CPU). DAW glue exists via **Neutone SDK**
   and **nn~/IRCAM**.
3. **Neural audio codecs** built on **residual vector quantization** (SoundStream
   → EnCodec/DAC → Mimi) provide the low-frame-rate discrete tokenization that
   makes LLM-style control of audio tractable.

**The principal gaps the framework could uniquely fill:** the **LLM
agent / natural-language-to-DSP-graph control plane** (verified evidence exists
for adjacent pieces — Text2FX, WavCraft — but no single working NL-to-graph
copilot), and **a single engine treating DSP and neural blocks as first-class
peers across both real-time and offline modes**.

**Known hard problems:** non-convex differentiable-oscillator optimization
(uninformative frequency gradients); recursive/IIR filters impeding
auto-differentiation; the strict real-time-safety constraints that make
mainstream PyTorch/TF unusable on the audio thread without specialized wrappers.

---

## Verified findings (8, all high-confidence)

### F1 — Differentiable DSP makes classic DSP + neural nets composable peers
DDSP backpropagates loss gradients through digital signal processors, so DSP
modules can be embedded inside and trained end-to-end with neural nets, achieving
high-fidelity synthesis **without** large autoregressive models or adversarial
(GAN) losses. *Caveat:* in canonical DDSP the DSP block is typically a decoder
driven by network-predicted parameters rather than a fully symmetric peer with
its own weights.
**Sources:** arXiv:2001.04643 (DDSP, Engel et al., ICLR 2020); Frontiers DDSP
review (Hayes et al. 2023/2024); arXiv:2308.15422. **Vote:** 3-0.

### F2 — Differentiable sinusoidal-oscillator optimization is a real hard problem
Gradients from most audio/spectral loss functions are **uninformative about
ground-truth frequency**; naive gradient descent over an audio loss does not
solve frequency estimation. Requires workarounds (self-supervised pretraining,
complex-exponential surrogates / Wirtinger derivatives).
**Sources:** Frontiers DDSP review; arXiv:2012.04572 (Turian & Henry, "I'm Sorry
for Your Loss"); arXiv:2210.14476 (Hayes et al., "Sinusoidal Frequency
Estimation by Gradient Descent"). **Vote:** 3-0.

### F3 — RAVE demonstrates the full real-time-neural-in-a-DSP-host pattern
RAVE (IRCAM ACIDS) is a VAE for fast, high-quality neural audio synthesis. A
`--streaming` export flag enables **cached convolutions** for artifact-free
real-time/causal processing; the model integrates via the **nn~** external for
Max/MSP and PureData, and as a VST2/VST3/AU plugin (Ableton Live, Bitwig).
**Sources:** github.com/acids-ircam/RAVE; arXiv:2111.05011; arXiv:2204.07064
(Streamable Neural Audio Synthesis With Non-Causal Convolutions);
github.com/acids-ircam/cached_conv; github.com/acids-ircam/nn_tilde. **Vote:** 3-0.

### F4 — RVQ neural codecs are the tokenization layer for LLM-style audio control
SoundStream (2021) introduced the pattern: convolutional encoder/decoder + RVQ,
trained jointly end-to-end, **real-time on a smartphone CPU**. This recipe
underpins EnCodec and DAC. **Mimi** (Kyutai) extends it: low **12.5 fps** frame
rate (~10× lower than a 125 fps codec, directly shortening LLM token sequences),
32 RVQ levels with randomly-truncated reconstruction during training, and a
split into **WavLM-distilled semantic tokens** + acoustic tokens (Moshi weights
the semantic loss 100×). *Caveat:* the 54 GB vs 134 GB dataset figure is a ~2.5×
reduction (16/32 codebooks offset the frame-rate drop), not 10×.
**Sources:** arXiv:2107.03312 (SoundStream); kyutai.org/codec-explainer (Mimi);
arXiv:2410.00037 (Moshi); arXiv:2402.13071 (Codec-SUPERB). **Vote:** 3-0.

### F5 — MIDI-DDSP shows hierarchical, disentangled, editable neural-synth control
MIDI-DDSP (Google Magenta, ICLR 2022) builds a three-level hierarchy (notes →
performance attributes → DDSP synthesis parameters), letting users intervene at
any level or use trained priors, and independently manipulate high-level
expressive attributes (timbre, vibrato, dynamics, articulation).
**Sources:** arXiv:2112.09312. **Vote:** 3-0.

### F6 — RTNeural is the de facto C++ inference engine for hard-real-time audio
Allocates **all** memory at model-load time (mainstream PyTorch/TF rely on
dynamically resizable structures like `std::vector` that allocate during
inference → unsafe on the audio thread). Eigen/XSIMD/STL backends (+ experimental
Apple Accelerate), run-time and compile-time APIs; layers Dense, GRU, LSTM,
Conv1D/2D, MaxPooling, BatchNorm. Benchmarks (48 kHz, i7-9750H): beats the
PyTorch C++ API for smaller layers (compile-time API fastest via loop
unrolling); PyTorch scales better to large layers. It is the engine behind
NAM/GuitarML neural amp modeling.
**Sources:** github.com/jatinchowdhury18/RTNeural; arXiv:2106.03037;
ccrma.stanford.edu/~jatin/rtneural. **Vote:** 3-0 (one layer-list sub-claim 2-1).

### F7 — ANIRA: one real-time-safe inference layer wrapping multiple ML backends
ANIRA (TU Berlin, IEEE IS² 2024) wraps **LibTorch, ONNX Runtime, TensorFlow
Lite/LiteRT** behind one C++ abstraction selectable at runtime, achieving
real-time safety by **decoupling inference from the audio callback into a static
thread pool** (avoids oversubscription, deterministic runtimes). Inference runs
**off**, not on, the audio thread.
**Sources:** github.com/anira-project/anira; arXiv:2506.12665. **Vote:** 3-0.

### F8 — Neutone SDK is the PyTorch-to-DAW deployment layer
Lets researchers work entirely in Python while a **model-agnostic** interface
streamlines deployment of PyTorch neural-audio models for **both real-time and
offline** use, encapsulating variable buffer sizes, sample-rate conversion,
delay compensation, control-parameter handling. Names the exact target gap:
"integrating DL models into DAWs remains challenging due to real-time inference
constraints and the complexities of plugin development." *Caveat:*
self-description of design intent, not independent evaluation.
**Sources:** arXiv:2508.09126; github.com/Neutone/neutone_sdk. **Vote:** 3-0.

---

## Additional source-level claims (fetched, not collapsed into F1–F8)

These came from individual source extractors and are useful leads; they were
verified at the source level but not all promoted into the final synthesized set.

- **Text2FX** (arXiv:2409.18847, ICASSP 2025, Northwestern/Adobe): maps
  open-vocabulary NL prompts ("make this in-your-face and bold", brightness /
  reverb prompts) to **audio-effect parameters (EQ, reverb)** by combining
  **CLAP** text-audio embeddings with **differentiable DSP**. The most direct
  published precedent for the NL-to-DSP-control plane.
- **WavCraft** (arXiv:2403.09527): uses an LLM as a **control plane** that
  decomposes a user's NL instruction into sub-tasks, each dispatched to a
  task-specific audio model — the NL-to-pipeline / tool-orchestration pattern.
- **BRAVE** (arXiv:2503.11562): "Bravely Realtime Audio Variational
  autoEncoder" — **<10 ms** total latency (10.08 ms Filosax, 9.75 ms Drumset),
  ~25–45× lower than non-causal RAVE v1 (244.83 / 439.89 ms).
- **LLVC** (arXiv:2311.00873): low-latency voice conversion, **<20 ms** at 16 kHz.
- **Differentiable IIR / all-pole filters** (arXiv:2404.07970): the recursive
  structure of IIR/all-pole filters impedes end-to-end AD; frequency-sampling /
  frame-based workarounds don't accurately reflect the true recursive gradient.
- **Differentiable music-mixing-graph search** (arXiv:2406.01049): a music
  mixing graph (a DAG of audio processors) can be **reverse-engineered from
  input/output audio pairs** via a fully differentiable implementation of both
  the processors and the pruning — gradient-based search instead of discrete
  combinatorial graph search. Directly relevant to a hybrid graph engine.
- **cached_conv** (acids-ircam.github.io/cached_conv): non-causal-trained models
  can be **converted to streaming/real-time** via post-training reconfiguration,
  avoiding the quality loss of specialized causal training.

---

## Caveats (from the research pass)

- Confidence is high across all 8 findings; every claim anchors to a primary
  source; 24/25 verified 3-0.
- **Scope limitation — NOT covered by the verified claim set** (absence of a
  claim ≠ evidence of absence; these need their own research pass):
  - The **LLM agent / NL-to-DSP-graph control plane** as an end-to-end working
    system (Text2FX and WavCraft are adjacent, not a full graph copilot).
  - **Source separation** (Demucs / Spleeter / Open-Unmix).
  - **Neural vocoders** (HiFi-GAN).
  - **Neural effects / amp modeling** research (only NAM/RTNeural *deployment*
    is covered).
  - **Diffusion/transformer generation** (MusicGen, Stable Audio).
  - **Classic DSP frameworks** (Faust, Max/MSP, PureData, SuperCollider, JUCE,
    Web Audio, the Rust audio ecosystem).
  - **Python+C++ interop tooling** (pybind11 / nanobind).
  - **Datasets** (MUSDB18).
  - **Audio-quality evaluation metrics**.
- **Time-sensitivity:** field moves fast (Mimi/Moshi 2024; ANIRA & Neutone SDK
  papers 2025). RTNeural-vs-PyTorch crossover is tied to 2019-era hardware /
  2021 library versions.
- Several supporting papers (RTNeural, Neutone) are authored by the tools'
  own creators → mild self-interest on performance/ease claims (factual
  descriptions independently verifiable).

---

## Open questions (carried into `60-gaps-and-opportunities.md`)

1. Does any working system actually compile **natural-language intent into
   DSP/neural signal graphs**? Strong evidence for the DSP/neural and real-time
   layers; **none** for a functioning NL-to-graph audio copilot. Biggest
   evidence gap → clearest differentiating opportunity.
2. What is the right **signal-graph engine architecture** (lazy vs eager,
   dataflow scheduling) and **Python+C++ interop** strategy for one engine that
   treats DSP and neural blocks as first-class peers across **both** real-time
   (<10–20 ms) and offline modes? No verified claim addresses this directly.
3. How do leading **source-separation, vocoder, neural-effects, and
   diffusion/transformer** systems fit the real-time vs offline split, and which
   can meet a <10–20 ms streaming/causal budget vs being inherently offline?
4. Which **datasets** (MUSDB18 and beyond) and **objective+subjective metrics**
   (SI-SDR, ViSQOL, FAD, MUSHRA/MOS) should the framework adopt, and how to
   choose differentiable-DSP losses given the uninformative-gradient problem?

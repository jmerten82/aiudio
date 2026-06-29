# 20 — Differentiable DSP & Neural Audio

How to make classic DSP and neural models *composable, trainable peers* — the
technical heart of the aiudio vision (pillar 1 + pillar 3).

> **Provenance key:** **✓ Verified** = cited from the deep-research pass (primary
> sources, adversarially verified). **○ Background** = well-established knowledge
> not part of the verified pass; treated as a lead to confirm, not settled fact.

---

## 1. Differentiable DSP (DDSP) — the foundational mechanism ✓

**DDSP** (Engel, Hantrakul, Gu, Roberts — Google Magenta, ICLR 2020,
[arXiv:2001.04643](https://arxiv.org/abs/2001.04643)) is *the* proven mechanism
for the framework's core idea. It backpropagates loss-function gradients
**through digital signal processors**, so classic DSP modules (additive/harmonic
oscillators, filtered noise, IIR filters, reverb) can be embedded inside and
trained end-to-end with neural networks. It achieves high-fidelity synthesis
**without** large autoregressive models *or* adversarial (GAN) losses — by
injecting strong DSP inductive bias.

The peer-reviewed survey — **"A Review of Differentiable Digital Signal
Processing for Music and Speech Synthesis"** (Hayes, Saitis, Fazekas et al.,
Frontiers in Signal Processing, 2023/2024,
[link](https://www.frontiersin.org/journals/signal-processing/articles/10.3389/frsip.2023.1284100/full))
— is the canonical reference and the best single source on the open problems.
It defines the field: *"differentiable digital signal processing describes a
family of techniques in which loss function gradients are backpropagated through
digital signal processors, facilitating their integration into neural
networks."*

> **Important nuance (✓, from the verification caveat):** in *canonical* DDSP the
> DSP block is usually a **decoder driven by network-predicted parameters**, not
> a fully symmetric peer with its own trainable weights. aiudio's "first-class
> peers" goal is therefore an *extension* of DDSP, not a restatement of it — a
> genuine design contribution if achieved cleanly.

### Design implication for aiudio
The DDSP pattern gives the *contract* for a hybrid node: a block exposes
(a) a forward audio render and (b) differentiable parameters. If every node —
classic or neural — honors that contract, the graph is trainable end-to-end.
This is the spine of the engine in `50-architecture-patterns.md`.

---

## 2. The hard problems of differentiable DSP ✓

These are concrete, verified design risks — not hypotheticals.

### 2.1 Oscillator frequency gradients are uninformative
Optimizing differentiable **sinusoidal oscillators w.r.t. frequency** is
non-convex, and gradients from most audio/spectral loss functions are
**uninformative about the ground-truth frequency**. Naive gradient descent over
an audio loss *does not* solve frequency estimation.
- Diagnosis: Turian & Henry, *"I'm Sorry for Your Loss: Spectrally-Based Audio
  Distances Are Bad at Pitch"* ([arXiv:2012.04572](https://arxiv.org/abs/2012.04572)).
- Workarounds: self-supervised pretraining; complex-exponential surrogates /
  Wirtinger derivatives — Hayes et al., *"Sinusoidal Frequency Estimation by
  Gradient Descent"* ([arXiv:2210.14476](https://arxiv.org/abs/2210.14476)).
- **Takeaway:** don't expect a naive multi-scale spectral loss to train
  pitch/frequency parameters. Budget for pitch-aware losses or staged training.

### 2.2 Recursive / IIR filters impede auto-differentiation ✓
The **recursive structure of IIR / all-pole filters** impedes end-to-end
training via autodiff; common workarounds (frequency sampling, frame-based
processing) **cannot accurately reflect the gradient** of the original recursive
system ([arXiv:2404.07970](https://arxiv.org/abs/2404.07970)). Time-varying
recursive filters (the useful kind for audio) are the hard case.
- **Takeaway:** differentiable biquads/EQ/reverb need custom backward passes or
  carefully chosen approximations; this is a real engineering line-item.

---

## 3. Neural synthesis & timbre transfer ✓

### RAVE — the reference real-time neural synth
**RAVE** ("A variational autoencoder for fast and high-quality neural audio
synthesis", Caillon & Esling, IRCAM ACIDS,
[arXiv:2111.05011](https://arxiv.org/abs/2111.05011),
[repo](https://github.com/acids-ircam/RAVE)) is the most important practical
reference for *neural synthesis that runs in a DSP host*:
- A `--streaming` export flag enables **cached convolutions** for artifact-free
  real-time/causal inference.
- Streaming mechanism detailed in *"Streamable Neural Audio Synthesis With
  Non-Causal Convolutions"* ([arXiv:2204.07064](https://arxiv.org/abs/2204.07064))
  and [`cached_conv`](https://github.com/acids-ircam/cached_conv): models
  **trained non-causally** can be **converted to streaming** post-hoc, avoiding
  the quality loss of specialized causal training.
- Integrates into Max/MSP & PureData via **nn~**
  ([nn_tilde](https://github.com/acids-ircam/nn_tilde)) and into DAWs as
  VST2/VST3/AU.

**BRAVE** ("Bravely Realtime Audio Variational autoEncoder",
[arXiv:2503.11562](https://arxiv.org/html/2503.11562v2)) pushes RAVE's idea to
**<10 ms** total latency (10.08 ms Filosax, 9.75 ms Drumset) — ~25–45× lower than
non-causal RAVE v1 — while improving pitch/loudness replication. Evidence that
real-time neural *timbre transfer* under the framework's latency budget is
achievable today.

### MIDI-DDSP — hierarchical, editable, disentangled control ✓
**MIDI-DDSP** (Wu et al., Google Magenta, ICLR 2022,
[arXiv:2112.09312](https://arxiv.org/pdf/2112.09312)) builds a **3-level
hierarchy** — notes → performance attributes → DDSP synthesis parameters — on top
of *interpretable* DDSP parameters. Users can intervene at **any** level or use
trained priors, and independently manipulate timbre, vibrato, dynamics,
articulation. This is the precedent for aiudio's human/agent-in-the-loop control
over neural synthesis: expose interpretable handles at multiple altitudes.

---

## 4. Neural audio codecs — the tokenization layer ✓

Why a framework that wants LLM-style control should care about codecs: **residual
vector quantization (RVQ)** turns continuous audio into a short sequence of
**discrete tokens**, which is what makes transformer/LLM modeling and control of
audio tractable.

| Codec | Key idea | Why it matters |
|---|---|---|
| **SoundStream** (2021, [arXiv:2107.03312](https://arxiv.org/abs/2107.03312)) ✓ | Fully-convolutional encoder/decoder + RVQ, trained jointly end-to-end; **real-time on a smartphone CPU**, streamable | The foundational RVQ recipe; low-latency + streamable proves codecs aren't inherently offline |
| **EnCodec** (Meta) ○ | RVQ codec w/ transformer entropy coding | Powering MusicGen/AudioGen token streams |
| **DAC** (Descript Audio Codec) ○ | Higher-fidelity RVQ at lower bitrate | Common modern choice for high-quality tokenization |
| **Mimi** (Kyutai, [explainer](https://kyutai.org/codec-explainer/)) ✓ | **12.5 fps** frame rate (~10× lower → shorter LLM sequences); 32 RVQ levels w/ randomly-truncated reconstruction; **split into WavLM-distilled semantic + acoustic tokens** (Moshi weights semantic loss 100×) | Lowest-frame-rate streamable codec; the semantic/acoustic split is a strong design pattern |

> **✓ Caveat:** Mimi's "54 GB vs 134 GB" dataset figure is a ~2.5× reduction, not
> 10× — the 16/32 RVQ codebooks offset the frame-rate drop. Don't conflate
> frame-rate ratio with token-volume ratio.

Supporting: **Moshi** ([arXiv:2410.00037](https://arxiv.org/abs/2410.00037)),
**Codec-SUPERB** benchmark ([arXiv:2402.13071](https://arxiv.org/abs/2402.13071)).

**Design implication:** aiudio should treat a pluggable neural codec as a
first-class graph primitive — it's the bridge between the waveform domain
(real-time DSP) and the token domain (agent/LLM reasoning, generation).

---

## 5. Source separation ○ (background — not in verified pass)

> The verified research pass did **not** cover separation; the following is
> established background to confirm before relying.

Stem/source separation is core to "music production first." The lineage:
- **Open-Unmix (UMX)** — open, reproducible BLSTM baseline on spectrograms.
- **Spleeter** (Deezer) — fast, widely-used U-Net spectrogram separator (2/4/5
  stems); the popular on-ramp.
- **Demucs / Hybrid Demucs / HT-Demucs** (Meta) — waveform-domain (and hybrid
  time/spectral, then transformer) models; long the quality leader on MUSDB18.
- Standard benchmark: **MUSDB18 / MUSDB18-HQ**; standard metric: **SI-SDR /
  SDR** (see `90-references.md` and §7 below).
- **Real-time caveat:** the strongest separators are non-causal/offline. Causal,
  low-latency separation is an active, harder sub-problem — relevant to whether
  separation can be a *real-time* node or an *offline-only* node in aiudio.

---

## 6. Neural effects / amp modeling, vocoders, and generative models ○ (background)

> Background; the verified pass covered only the *deployment* of amp models
> (NAM via RTNeural), not the modeling research.

- **Neural amp/effect modeling:** **NAM (Neural Amp Modeler)** and
  **GuitarML** model nonlinear gear (amps, pedals) from input/output pairs;
  these are the canonical *real-time neural effect* success story and run on
  RTNeural (see `30-realtime-neural-inference.md`). Closely related research:
  black-box/grey-box differentiable modeling of compressors, distortion, EQ.
- **Neural vocoders:** **HiFi-GAN**, WaveNet/WaveRNN (autoregressive, slow),
  **DDSP vocoders** — mel-spectrogram → waveform. HiFi-GAN is the common
  real-time-capable GAN vocoder.
- **Generative (mostly offline/batch):**
  - **MusicGen** (Meta) — transformer LM over EnCodec tokens, text-conditioned
    music.
  - **Stable Audio** (Stability) — latent diffusion for text-to-audio/music,
    notable for longer coherent generations.
  - **AudioLDM / Tango / Jukebox / MusicLM** — the broader text-to-audio family.
  - These are **inherently offline** at quality settings — they inform aiudio's
    *offline/generation* path, not its real-time path.

**Design implication:** aiudio's "configurable real-time vs offline" axis maps
cleanly onto this split — separation/generation default to offline nodes;
RAVE/BRAVE-style synthesis, NAM effects, and streamable codecs can be real-time
nodes.

---

## 7. Datasets & evaluation metrics ○ (background — not in verified pass)

> The verified pass flagged these as *requested but uncovered*. Background list
> to confirm.

**Datasets:** MUSDB18 / MUSDB18-HQ (separation); MAESTRO (piano,
audio+MIDI — DDSP/MIDI-DDSP heritage); NSynth (instrument notes); Slakh2100
(synthesized multitrack); URMP; MedleyDB; FMA; AudioSet (general). For speech
(secondary): LibriSpeech, Libri-Light, VCTK, DNS-Challenge.

**Objective metrics:**
- Separation/enhancement: **SI-SDR / SI-SDRi**, SDR, PESQ, STOI.
- Codec/synthesis fidelity: **ViSQOL**, **PESQ**, mel-cepstral distortion (MCD),
  multi-scale spectral loss (also used as a *training* loss).
- Generation/distribution similarity: **FAD** (Fréchet Audio Distance), KL over
  audio classifiers, CLAP-score for text-audio alignment.

**Subjective metrics:** **MOS** (mean opinion score), **MUSHRA**, ABX. Still the
gold standard for perceptual quality; expensive but decisive.

> **✓ Cross-link to F2:** choosing *training* losses is constrained by the
> uninformative-gradient problem — multi-scale spectral loss is poor at pitch.
> The framework's loss library should make pitch-aware / perceptual losses easy
> to compose.

---

## 8. What this means for aiudio

1. **The hybrid-peer node contract** (forward render + differentiable params) is
   validated by DDSP and extends it. ✓
2. **Real-time neural synthesis under the latency budget exists today**
   (RAVE-streaming, BRAVE) — adopt cached-convolution / streaming-export
   patterns. ✓
3. **A pluggable RVQ codec** is the waveform↔token bridge for the agent/LLM and
   generative paths. ✓
4. **Plan for the hard cases up front:** pitch-aware losses (F2), custom IIR
   backward passes (§2.2), and an explicit real-time-vs-offline classification
   per node type (§6). ✓/○

See `30-realtime-neural-inference.md` for how these run in real time, and
`60-gaps-and-opportunities.md` for where aiudio uniquely wins.

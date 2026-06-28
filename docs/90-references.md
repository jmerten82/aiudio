# 90 — References

Consolidated, deduplicated reference list for the aiudio dossier.

**Legend:**
- **[V]** — primary source **fetched & adversarially verified** in the
  deep-research pass (high confidence).
- **[S]** — supporting source cited *within* a verified finding's evidence
  (corroborating, not separately vote-verified).
- **[B]** — **background** reference added from established knowledge (the
  deep-research pass did *not* verify it); confirm before relying.

---

## Differentiable DSP & neural synthesis

- **[V] DDSP: Differentiable Digital Signal Processing** — Engel, Hantrakul, Gu,
  Roberts (Google Magenta), ICLR 2020. https://arxiv.org/abs/2001.04643
- **[V] A Review of Differentiable Digital Signal Processing for Music and Speech
  Synthesis** — Hayes, Saitis, Fazekas et al., Frontiers in Signal Processing,
  2023/2024. https://www.frontiersin.org/journals/signal-processing/articles/10.3389/frsip.2023.1284100/full
- **[V] MIDI-DDSP: Detailed Control of Musical Performance via Hierarchical
  Modeling** — Wu et al. (Magenta), ICLR 2022. https://arxiv.org/pdf/2112.09312
- **[S] DDSP-related (additive/amp)** — https://arxiv.org/abs/2308.15422
- **[S] I'm Sorry for Your Loss: Spectrally-Based Audio Distances Are Bad at
  Pitch** — Turian & Henry, 2020. https://arxiv.org/abs/2012.04572
- **[S] Sinusoidal Frequency Estimation by Gradient Descent** — Hayes et al.,
  2022. https://arxiv.org/abs/2210.14476
- **[V] Differentiable IIR / all-pole filters (recursive autodiff)** —
  https://arxiv.org/abs/2404.07970

## Neural synthesis engines (real-time-capable)

- **[V] RAVE: A Variational Autoencoder for Fast and High-Quality Neural Audio
  Synthesis** — Caillon & Esling (IRCAM ACIDS), 2021. https://arxiv.org/abs/2111.05011
  · repo https://github.com/acids-ircam/RAVE
- **[S] Streamable Neural Audio Synthesis With Non-Causal Convolutions** —
  Caillon & Esling, 2022. https://arxiv.org/abs/2204.07064
- **[V] cached_conv** (streaming export for non-causal models) —
  https://acids-ircam.github.io/cached_conv/ · https://github.com/acids-ircam/cached_conv
- **[V] BRAVE: Bravely Realtime Audio Variational autoEncoder** (<10 ms) —
  https://arxiv.org/html/2503.11562v2
- **[V] LLVC: Low-latency Low-resource Voice Conversion** (<20 ms @16 kHz) —
  https://arxiv.org/pdf/2311.00873

## Neural audio codecs (RVQ / tokenization)

- **[V] SoundStream: An End-to-End Neural Audio Codec** — Zeghidour et al.
  (Google), IEEE/ACM TASLP, 2021. https://arxiv.org/abs/2107.03312
- **[V] Mimi codec explainer** (Kyutai) — https://kyutai.org/codec-explainer/
- **[S] Moshi** (Kyutai; uses Mimi) — https://arxiv.org/abs/2410.00037
- **[S] Codec-SUPERB** (codec benchmark) — https://arxiv.org/abs/2402.13071
- **[B] EnCodec** (Meta) — Défossez et al. https://arxiv.org/abs/2210.13438
- **[B] DAC: Descript Audio Codec** — Kumar et al. https://arxiv.org/abs/2306.06546

## Real-time neural inference engines & deployment

- **[V] RTNeural** — Chowdhury (CCRMA). repo https://github.com/jatinchowdhury18/RTNeural
  · paper https://arxiv.org/pdf/2106.03037 · https://ccrma.stanford.edu/~jatin/rtneural
- **[V] ANIRA: Architecture for Neural Network Inference in Real-Time Audio** —
  TU Berlin, IEEE IS² 2024. repo https://github.com/anira-project/anira ·
  paper https://arxiv.org/abs/2506.12665
- **[V] Neutone SDK** — Mitcheltree et al., AES 2025. paper
  https://arxiv.org/abs/2508.09126 · repo https://github.com/Neutone/neutone_sdk
- **[V] nn~ (nn_tilde)** — IRCAM. https://github.com/acids-ircam/nn_tilde
- **[B] NAM (Neural Amp Modeler)** — https://github.com/sdatkinson/NeuralAmpModelerCore
- **[B] GuitarML** — https://github.com/GuitarML

## AI agents / copilots for audio

- **[V] Text2FX: Harnessing CLAP Embeddings for Text-Guided Audio Effects** —
  Northwestern + Adobe, ICASSP 2025. https://arxiv.org/abs/2409.18847
- **[V] WavCraft** (LLM-orchestrated audio task decomposition) —
  https://arxiv.org/abs/2403.09527
- **[B] CLAP: Contrastive Language-Audio Pretraining** — Wu et al. / LAION.
  https://arxiv.org/abs/2211.06687

## Differentiable graph / mixing

- **[V] Differentiable music-mixing-graph search (pruning)** —
  https://arxiv.org/pdf/2406.01049

## Generative audio models [B] (background — not verified)

- **MusicGen / AudioCraft** (Meta) — https://arxiv.org/abs/2306.05284
- **Stable Audio** (Stability AI) — https://stability.ai/research (latent
  diffusion for text-to-audio)
- **MusicLM** (Google) — https://arxiv.org/abs/2301.11325
- **AudioLDM** — https://arxiv.org/abs/2301.12503
- **Jukebox** (OpenAI) — https://arxiv.org/abs/2005.00341

## Source separation [B] (background — not verified)

- **Demucs / Hybrid Transformer Demucs** (Meta) —
  https://github.com/facebookresearch/demucs
- **Spleeter** (Deezer) — https://github.com/deezer/spleeter
- **Open-Unmix (UMX)** — https://github.com/sigsep/open-unmix-pytorch

## Vocoders [B] (background — not verified)

- **HiFi-GAN** — Kong et al. https://arxiv.org/abs/2010.05646

## Classic DSP / computer-music frameworks [B] (background — not verified)

- **Faust** — functional DSP language/compiler. https://faust.grame.fr/
- **Max/MSP** (Cycling '74) — https://cycling74.com/
- **Pure Data (Pd)** — https://puredata.info/
- **SuperCollider** — https://supercollider.github.io/
- **Web Audio API** (W3C) — https://www.w3.org/TR/webaudio/
- **JUCE** — https://juce.com/
- **Essentia** (MTG-UPF) — https://essentia.upf.edu/
- **librosa** — https://librosa.org/
- **torchaudio** — https://pytorch.org/audio/

## Rust audio ecosystem [B] (background — not verified)

- **CPAL** https://github.com/RustAudio/cpal · **FunDSP**
  https://github.com/SamiPerttu/fundsp · **dasp**
  https://github.com/RustAudio/dasp · **nih-plug**
  https://github.com/robbert-vdh/nih-plug · **Glicol** https://glicol.org/

## Python ↔ C++ interop [B] (background — not verified)

- **nanobind** — https://github.com/wjakob/nanobind
- **pybind11** — https://github.com/pybind/pybind11

## Plugin standards [B] (background — not verified)

- **CLAP** (open) — https://cleveraudio.org/ · https://github.com/free-audio/clap
- **VST3** (Steinberg) — https://www.steinberg.net/developers/
- **LV2** — https://lv2plug.in/

## Datasets [B] (background — not verified)

- **MUSDB18 / MUSDB18-HQ** — https://sigsep.github.io/datasets/musdb.html
- **MAESTRO** — https://magenta.tensorflow.org/datasets/maestro
- **NSynth** — https://magenta.tensorflow.org/datasets/nsynth
- **Slakh2100** — http://www.slakh.com/

## Evaluation metrics [B] (background — not verified)

- **SI-SDR** — Le Roux et al. https://arxiv.org/abs/1811.02508
- **Fréchet Audio Distance (FAD)** — Kilgour et al. https://arxiv.org/abs/1812.08466
- **ViSQOL** — https://github.com/google/visqol
- **MUSHRA** (ITU-R BS.1534) · **MOS / PESQ / STOI** — perceptual/speech-quality
  standards.

---

### Verification summary (deep-research pass, 2026-06-28)

- 5 search angles · 18 sources fetched · 89 claims extracted · **25 claims
  adversarially verified (3-vote)** · **25/25 confirmed, 0 killed** · 8
  synthesized findings · 100 agent calls.
- All **[V]** entries trace to a primary source that passed verification (24/25
  at 3-0; one RTNeural layer-list sub-claim at 2-1).
- **[B]** entries were **not** part of the verified set — they fill explicitly
  uncovered topics and should be confirmed in a follow-up research pass (see
  `60-gaps-and-opportunities.md` §5).

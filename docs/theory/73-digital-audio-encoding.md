# 73 — How Audio Is Encoded in Digital Systems (with aiudio references)

A primer on how sound becomes numbers in a digital audio system — sampling,
quantization, PCM, integer vs. float, channels/interleaving, blocks, data
rates — and **how aiudio's I/O pipeline implements each concept**. Every section
ends with a *"→ in aiudio"* pointer to the actual code.

> This is established DSP background, not from the research pass. The aiudio
> references describe the pipeline as built through the I/O layer (M0–M4;
> full-duplex is M4). Confirm exact symbols against the headers when in doubt.

---

## 1. The big picture: continuous sound → discrete numbers

Sound is a **continuous** pressure wave: continuous in *time* and continuous in
*amplitude*. A computer can store neither continuum, so an **ADC**
(analog-to-digital converter) does two discretizations, and a **DAC** reverses
them on the way out:

1. **Sampling** — measure the amplitude at regular time intervals (discretize
   time).
2. **Quantization** — round each measurement to one of a finite set of levels
   (discretize amplitude).

The result is **PCM** (Pulse-Code Modulation): a stream of numbers, one (per
channel) per time step. Everything else in this document is detail on those two
axes plus how the numbers are laid out in memory.

> **→ in aiudio:** the ADC/DAC live in the audio hardware; Core Audio hands us
> the already-sampled, already-quantized PCM stream at the device boundary
> (`src/io/coreaudio_*backend.cpp`). aiudio is a *digital* pipeline — it operates
> entirely on PCM numbers.

---

## 2. Sampling — the time axis

### Sample rate
The number of samples taken per second, in hertz. Common rates:
- **44 100 Hz** — CD; historically tied to video equipment.
- **48 000 Hz** — the professional/video standard. **aiudio's default.**
- 88.2k / 96k / 176.4k / 192k — high-resolution production.

### Nyquist–Shannon: why 48 kHz is "enough"
To represent a signal without ambiguity you must sample at **more than twice** the
highest frequency present. The **Nyquist frequency** is therefore `sampleRate / 2`:

- 48 kHz → Nyquist = **24 kHz**, comfortably above the ~20 kHz upper limit of human
  hearing.

If frequencies above Nyquist reach the ADC they fold back as false low
frequencies — **aliasing** — so converters apply an anti-aliasing (low-pass)
filter before sampling, and a reconstruction filter on output. (Aliasing is also
why DSP that creates harmonics — distortion, nonlinear neural effects — must take
care not to exceed Nyquist.)

> **→ in aiudio:** sample rate is carried in `StreamConfig.sampleRate` (setup)
> and per block in `TimeInfo` (`sampleTime`, `hostTimeSeconds`) — it is **not**
> stored in the audio buffer itself. The backends read the device's actual rate
> during `open()` (sample-rate "negotiation"). The pipeline currently assumes one
> rate end-to-end; **resampling between rates is future work (M9)**.

---

## 3. Quantization — the amplitude axis

### Bit depth and levels
Each sample is stored with a fixed number of bits. **N bits → 2ᴺ distinct
amplitude levels.** Rounding to the nearest level introduces **quantization
error** (heard as noise). The signal-to-noise / dynamic range of linear PCM is
approximately:

```
dynamic range (dB) ≈ 6.02 · N + 1.76
```

- **16-bit** → 65 536 levels → ≈ **96 dB** dynamic range (CD quality).
- **24-bit** → ≈ **144 dB** (studio capture; exceeds analog noise floors).
- **32-bit float** → see §4.

**Dither** — adding a tiny amount of shaped noise *before* reducing bit depth —
decorrelates quantization error so it sounds like benign hiss instead of harmonic
distortion. (aiudio's current float→int16 conversion does **not** dither; it's a
simple round/truncate — a known simplification for later.)

> **→ in aiudio:** the engine doesn't quantize internally (it's float, §4).
> Quantization happens only at integer boundaries — e.g. when writing a 16-bit
> WAV via `floatToInt16()` in `examples/cpp/example_support.hpp` (`WavWriter`).

---

## 4. The number format: integer PCM vs. floating-point PCM

The *same* PCM samples can be stored as integers or floats.

### Integer PCM
Signed integers spanning the type's range:
- **int16**: `−32768 … +32767` (full scale = ±32768).
- int24, int32 also exist.
Compact and what most hardware/files use. Downside: a fixed ceiling — exceed it
and you **clip** (hard distortion); and gain changes lose precision.

### Floating-point PCM (32-bit `float`)
Samples are floats with **full scale at ±1.0** by convention. A 32-bit float has a
24-bit mantissa (≈ 144 dB instantaneous precision) *and* an exponent giving an
enormous total range — so:
- **No clipping between processing stages** — intermediate values can exceed ±1.0
  and come back down without damage (only the final conversion to int/the DAC
  clamps).
- **Uniform relative precision** across loud and quiet signals.
- Matches GPU/ML math (PyTorch tensors are float32) — important for aiudio's
  differentiable/neural nodes.

This is why virtually every modern audio *engine* works in float internally while
*files and devices* are often integer.

### dBFS — measuring level
Level is quoted in **decibels relative to full scale**: `dBFS = 20·log10(|x| / 1.0)`.
- 0 dBFS = full scale (±1.0).
- A full-scale sine has RMS `1/√2 ≈ 0.707` → **−3.01 dBFS**.
- Silence → −∞ dBFS.

> **→ in aiudio:** the engine's canonical sample type is **`float`, full-scale
> ±1.0** (`include/aiudio/io/audio_buffer.hpp`). Conversions to/from int16 live in
> `conversions.hpp`: `int16ToFloat()` divides by 32768; `floatToInt16()` **clamps
> to [−1, 1]** then scales by 32767 (so it can't wrap). The dBFS formula is what
> `ex_capture_meter` / `ex_device_probe` print — e.g. our captured-mic test
> measured **−36 dBFS** (real audio) vs. a silent path's −∞.

---

## 5. Channels, frames, and memory layout

### Sample vs. frame (a critical distinction)
- A **sample** is one number for one channel at one instant.
- A **frame** is one sample for *every* channel at one instant.
So a 1-second stereo 48 kHz clip is **48 000 frames** = **96 000 samples**.

> **→ in aiudio:** the code names these explicitly (CLAUDE.md §8). `AudioBuffer`
> carries `numChannels` and `numFrames`; block sizes are always in **frames**.

### Interleaved vs. planar (deinterleaved)
For multi-channel audio there are two memory layouts:
- **Interleaved**: channels alternate in one buffer — `L0 R0 L1 R1 L2 R2 …`
- **Planar / non-interleaved**: one contiguous buffer per channel —
  `[L0 L1 L2 …]` and `[R0 R1 R2 …]` separately.

Interleaved is common at hardware/file boundaries; planar is friendlier for
per-channel DSP and for ML tensors (shape `[channels, frames]`).

> **→ in aiudio:** the engine is **planar** — `AudioBuffer.channels[ch][frame]`.
> Core Audio devices may present *either* layout, so the backends bridge it:
> output points straight at the device's per-channel buffers when non-interleaved
> (zero-copy) or `interleave()`s planar scratch when interleaved; input
> `deinterleave()`s (see `coreaudio_backend.cpp` / `coreaudio_input_backend.cpp`).

### Endianness
Multi-byte integer samples have a byte order. WAV/AIFF specify it (WAV is
little-endian); the dev machine (Apple Silicon) is little-endian.

> **→ in aiudio:** `WavWriter` writes little-endian explicitly (byte-by-byte
> header + native int16 payload).

---

## 6. Blocks, buffers, and latency

Real-time audio isn't processed one sample at a time — it's processed in **blocks**
(a.k.a. buffers) of `B` frames. The hardware calls back roughly every `B / sampleRate`
seconds, hands you a block, and expects the next block back before the deadline.

- Block size is a **latency vs. overhead/safety** trade: smaller = lower latency
  but more callbacks and higher xrun (glitch) risk.
- **128 frames @ 48 kHz = 128 / 48000 ≈ 2.67 ms** per block.

> **→ in aiudio:** the block is `numFrames` in every `process(in, out, frames,
> time)` call; `StreamConfig.blockSize` defaults to **128**. The audio thread must
> finish each block within the deadline — hence the RT-safety rules (ADR-0004) and
> the lock-free `RingBuffer` for moving blocks to other threads.

---

## 7. Data rate (uncompressed)

```
bytes/second = sampleRate × (bitDepth / 8) × channels
```

- 48 kHz · 16-bit · stereo = **192 000 B/s = 1.536 Mbit/s** (CD-ish).
- 48 kHz · 32-bit float · stereo = **384 000 B/s = 3.072 Mbit/s** (engine-internal).
- aiudio's `capture.wav` (48 kHz · 16-bit · **mono**) = 96 000 B/s.

This is why storage/transmission often *compresses* (§8) even though engines run
uncompressed float internally.

---

## 8. Beyond linear PCM: compression and neural codecs

Linear PCM is simple but bulky. Other encodings trade computation for size:

- **Lossless** (FLAC, ALAC, WavPack) — exact reconstruction, ~2× smaller via
  entropy coding/prediction.
- **Lossy** (MP3, AAC, Opus, Vorbis) — discard perceptually-irrelevant detail
  using psychoacoustic models; far smaller, not bit-exact.
- **Neural audio codecs** (SoundStream, EnCodec, DAC, Mimi) — a learned
  encoder/decoder with **residual vector quantization** turns audio into a short
  sequence of **discrete tokens**. This is the "encoding" that makes LLM-style
  modeling and generation of audio tractable.

> **→ in aiudio:** the **waveform I/O pipeline documented here is linear PCM
> float32** end-to-end. The **token domain** (neural codecs / RVQ) is a separate,
> *future* layer — it's the bridge from waveforms to the agent/generation paths,
> covered in [`20-differentiable-dsp-and-neural-audio.md`](20-differentiable-dsp-and-neural-audio.md)
> §4. A neural-codec node would sit *inside* the graph, converting between the
> float32 waveform on its wires and tokens internally.

---

## 9. End-to-end: the format at each hop in aiudio

Tracing a live capture → process → playback path (full-duplex = M4):

```
mic ──► ADC ──► Core Audio input buffer            Core Audio output buffer ──► DAC ──► speaker
                float32, interleaved OR planar      float32, interleaved OR planar
                     │ deinterleave()                       ▲ interleave() (if needed)
                     ▼                                        │
        ┌───────── RenderCallback.process(in, out, numFrames, time) ──────────┐
        │  in / out : PLANAR float32, ±1.0, numChannels × numFrames (128)      │  ← engine lingua franca
        └──────────────────────────────────────────────────────────────────────┘
             │ cross-thread: RingBuffer<float>        offline sink: floatToInt16()
             ▼ float32                                ▼ int16 PCM, interleaved, little-endian
        consumer thread                               capture.wav  (16-bit mono)
```

| Concept | Representation | Where in aiudio |
|---|---|---|
| Sample type (engine) | 32-bit float, ±1.0 | `audio_buffer.hpp` (`AudioBuffer`) |
| Channel layout (engine) | planar `channels[ch][frame]` | `audio_buffer.hpp` |
| Block size | frames (128) | `StreamConfig.blockSize`, `numFrames` |
| Sample rate | Hz, in config/time (not in buffer) | `StreamConfig`, `TimeInfo` |
| Device format bridge | int16⇄float, interleave⇄planar | `conversions.hpp` / `conversions.cpp` |
| Device PCM | Core Audio float32 (either layout) | `coreaudio_backend.cpp`, `coreaudio_input_backend.cpp`, `coreaudio_duplex_backend.cpp` |
| Cross-thread transport | `RingBuffer<float>` | `ring_buffer.hpp` |
| File encoding | int16 PCM, interleaved, LE | `WavWriter` (`example_support.hpp`) |
| Level metering | dBFS = 20·log10(\|x\|) | `ex_capture_meter`, `ex_device_probe`, `ex_duplex_probe` |
| Token domain (future) | discrete RVQ tokens | `docs/theory/20-*` §4 (not in the I/O pipeline) |

---

## 10. What aiudio deliberately does *not* do (yet)

- **No internal resampling** — all stages assume the device rate; mixing rates is
  M9.
- **No double precision / fixed-point internally** — float32 only.
- **No dithering** on float→int16 (simple round/truncate) — fine for now, a
  refinement later.
- **No compressed/encoded waveform format** in the I/O path — linear PCM only; the
  neural-codec/token layer is separate and future.
- **No rich channel-layout semantics** — channels are positional (a count, not a
  labelled 5.1/7.1 map).

## 11. Glossary

- **PCM** — Pulse-Code Modulation; uncompressed sampled-and-quantized audio.
- **Sample / Frame** — one number for one channel / one sample across all channels.
- **Sample rate** — samples per second (Hz).
- **Bit depth** — bits per sample; sets the number of amplitude levels.
- **Nyquist frequency** — `sampleRate / 2`; the highest representable frequency.
- **Aliasing** — false frequencies from sampling content above Nyquist.
- **dBFS** — level in dB relative to full scale (0 dBFS = max).
- **Interleaved / Planar** — channels mixed in one buffer / split per channel.
- **Block (buffer)** — a chunk of frames processed per callback.
- **Dither** — noise added before bit-depth reduction to mask quantization error.
- **RVQ** — residual vector quantization; the basis of neural audio codecs.

## References
- `docs/pipeline/71-io-layer-milestones.md` (the I/O layer), `docs/pipeline/72-m1-aiudio-io-reference.md`
  (`aiudio-io` types), ADR-0003/0004/0005/0007/0008.
- `docs/theory/20-differentiable-dsp-and-neural-audio.md` §4 (neural codecs / token domain).

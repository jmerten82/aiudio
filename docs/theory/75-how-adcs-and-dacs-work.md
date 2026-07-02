# 75 — How ADCs and DACs Work (with aiudio references)

A primer on the **analog↔digital boundary** — the converters that turn sound into
numbers (ADC) and numbers back into sound (DAC). [`docs/theory/73`](73-digital-audio-encoding.md)
explains the *numbers* (sampling, quantization, PCM, float); this opens the
**black box** docs/theory/73 treats as external — how the conversion physically happens,
and **where each idea touches aiudio's pipeline**. Every section ends with a
*"→ in aiudio"* pointer.

> This is established electronics/DSP background, not from the research pass. The
> aiudio references describe the pipeline as built through the multi-source I/O
> work (Core Audio backends, the drift servo, latency reporting). Confirm exact
> symbols against the headers when in doubt. Companion to [`docs/theory/73`](73-digital-audio-encoding.md)
> and [`docs/pipeline/77`](../pipeline/77-combining-multiple-audio-io.md).

---

## 1. The boundary: where aiudio starts and stops

aiudio is a **digital** pipeline — it only ever handles PCM numbers. The
conversion to/from the physical, continuous world happens **just outside** it, in
two converters:

- **ADC** (analog-to-digital converter): microphone/line voltage → PCM samples.
- **DAC** (digital-to-analog converter): PCM samples → speaker/line voltage.

Everything docs/theory/73 describes (sample rate, bit depth, float ±1.0) is the
*language the converters speak on their digital side*. This doc is about what the
converters do to cross the line, because two of their properties — **their sample
clock** and **their latency** — reach directly into how aiudio schedules and
aligns audio.

> **→ in aiudio:** the converters are in the hardware; Core Audio hands the
> already-converted **float32** stream across the device boundary
> (`src/io/coreaudio_input_backend.cpp` = the ADC side,
> `src/io/coreaudio_backend.cpp` = the DAC side). The engine never sees volts —
> only the numbers the ADC produced or the numbers destined for the DAC.

---

## 2. The two conversions in one screen

Both directions are the same three ideas from docs/theory/73 (a **band-limiting filter**,
a **sample clock**, and a **quantizer**) arranged in opposite order:

```
  CAPTURE (ADC):   mic ──► anti-alias LPF ──► sample & hold ──► quantize ──► PCM in
                          (block > Nyquist)   (at Fs)          (to N bits)

  PLAYBACK (DAC):  PCM out ──► reconstruct ──► anti-imaging LPF ──► analog out ──► speaker
                              (hold/interp)    (remove images > Nyquist)
```

- The ADC must **remove** everything above Nyquist *before* sampling, or it
  aliases (docs/theory/73 §2). The DAC must **remove** the spectral **images** the
  discrete samples create *above* Nyquist, or you hear/aggravate ultrasonic junk.
- Both are anchored to a **sample clock** running at `Fs` (e.g. 48 kHz). That
  clock is the single most important thing this document adds to docs/theory/73 (§6).

> **→ in aiudio:** `StreamConfig.sampleRate` is that `Fs`, read from the device at
> `open()`. Anti-aliasing/anti-imaging live in the converter; aiudio's own
> responsibility is not to *create* content above Nyquist inside the graph (§9).

---

## 3. Inside an ADC

Conceptually an ADC is: **anti-alias filter → sample-and-hold → quantizer.**

1. **Anti-aliasing filter.** A low-pass that attenuates everything above Nyquist
   (`Fs/2`) so nothing folds back as a false low frequency. A perfect brick-wall
   is impossible, which is one reason modern converters **oversample** (below).
2. **Sample-and-hold.** At each tick of the sample clock, freeze the instantaneous
   voltage so the quantizer has a stable value to measure.
3. **Quantizer.** Round that voltage to the nearest of `2ᴺ` levels → an N-bit
   integer (docs/theory/73 §3). Rounding error = **quantization noise**.

### The modern reality: sigma-delta (ΔΣ) + oversampling
Almost no audio ADC quantizes straight to 24-bit at 48 kHz. Instead a **sigma-delta
modulator** samples at a *very high* rate (MHz — hundreds of times `Fs`) with only
**1 – a few bits**, using a feedback loop that **shapes the quantization noise**
out of the audio band (pushes it up to ultrasonic frequencies). A digital
**decimation filter** then low-pass-filters and downsamples that fast, coarse
stream to the clean `48 kHz / 24-bit` PCM you asked for. Net effect: high
resolution in the audio band from cheap 1-bit hardware, with the gentle analog
anti-alias filter doing far less work.

> **→ in aiudio:** all of this is finished before Core Audio hands us a block —
> we receive the decimated float32. Two consequences *do* leak through: the
> decimation filter adds **latency** (§7), and the modulator runs on the device's
> **crystal clock** (§6), which differs slightly between devices.

---

## 4. Inside a DAC

A DAC reverses the chain: **reconstruct → anti-imaging filter → output.**

1. **Reconstruction (hold/interpolate).** Turn the discrete samples back into a
   stepped voltage. The naïve version is a **zero-order hold** (hold each sample
   until the next) — which imprints a mild `sinc` high-frequency roll-off and
   creates spectral **images** (copies of the audio band mirrored around multiples
   of `Fs`).
2. **Anti-imaging (reconstruction) filter.** A low-pass that removes those images
   above Nyquist so only the intended band survives.
3. **Sigma-delta again (modern DACs).** Like ADCs, most audio DACs first
   **oversample/interpolate** the PCM up to a high rate, run a ΔΣ modulator to a
   1-bit stream with noise shaping, and let a simple analog filter smooth it —
   trading MHz-rate digital work for cheap, high-quality analog output.
4. **Output stage.** Analog gain/buffering drives the line or headphone/speaker.

The DAC is where the numbers finally become sound — and the **only** place the
signal is truly clamped to full scale (docs/theory/73 §4: float can exceed ±1.0
internally; the DAC cannot).

> **→ in aiudio:** the block the master output backend writes to `out` is what the
> DAC will reconstruct. Because the engine is float ±1.0, an internal over
> (>±1.0) is harmless *until* it reaches the DAC, which clips it — so a limiter
> before the sink (a `Compressor`, `docs/cookbooks/82` §4) protects the physical output.

---

## 5. Resolution, noise, and full scale

The same measures from docs/theory/73 §3–§4, now as *converter* specs:

- **Dynamic range / SNR** ≈ `6.02·N + 1.76` dB for N ideal bits (16-bit ≈ 96 dB,
  24-bit ≈ 144 dB). Real converters fall short — the honest figure is **ENOB**
  (effective number of bits), which folds in analog noise, jitter, and
  distortion.
- **Dither** — a tiny shaped noise added *before* the final quantization
  decorrelates the error so it sounds like benign hiss instead of harmonic
  distortion (docs/theory/73 §3). Good ADCs/DACs dither internally.
- **0 dBFS = the converter's full-scale clipping point.** "Headroom" is how far
  your peaks sit below it.

> **→ in aiudio:** the engine works in float (huge internal range), so precision
> is never the *engine's* bottleneck — the ADC's ENOB on the way in and the DAC's
> on the way out are. dBFS is what `ex_capture_meter` / a `Meter` node report
> (`g.meter_mean_square` → `10·log10`), measuring level against that same
> full-scale reference. aiudio's float→int16 path (`conversions.hpp`, used only at
> **WAV** boundaries — the live device is float32) does **not** dither yet
> (docs/theory/73 §10).

---

## 6. The sample clock — the heartbeat (and why it's aiudio's master clock)

Every converter is driven by a **crystal oscillator** ticking at (a multiple of)
`Fs`. Two facts about that clock dominate real-world multi-device audio:

- **Jitter** — tiny timing errors in *when* each sample is taken/emitted — smears
  the conversion and raises the noise floor. High-end gear spends real money on
  low-jitter clocks.
- **No two crystals are identical.** A device nominally at "48 000 Hz" might run
  at 48 000.6 Hz; another at 47 999.4 Hz. Over minutes this **drift** accumulates
  into whole samples.

This is exactly why an audio engine picks **one clock as the master** (the
converter whose IOProc fires the callback) and treats every other clock as
something to *reconcile*.

> **→ in aiudio:** this is the single deepest converter→pipeline connection.
> - **The converter's sample clock *is* the engine clock.** A backend's IOProc is
>   driven by the device's ADC/DAC clock; that is the "swappable clock" of
>   **ADR-0005**. The master in `LiveMultiSource` / `MasterClockAdapter` is
>   literally *some converter's crystal*.
> - **Cross-converter drift is why the drift servo exists.** Two devices = two
>   crystals = inevitable drift → aiudio brings each off-clock source onto the
>   master timeline through a `ResamplingSource` whose `DriftCompensator` servo
>   tracks the ratio and keeps the ring bounded (**ADR-0008/0015**,
>   [`docs/pipeline/76`](../pipeline/76-multi-source-io-roadmap.md)/[`docs/pipeline/77`](../pipeline/77-combining-multiple-audio-io.md)).
>   The 44 100/48 000 examples in [`docs/cookbooks/81`](../cookbooks/81-pipeline-usage-patterns.md) are
>   the *nominal-rate* case of the same machinery that also absorbs crystal drift.

---

## 7. Conversion latency

Those decimation (ADC) and interpolation/reconstruction (DAC) filters are digital
FIR/IIR filters, so they have **group delay** — the converter itself adds latency,
on top of the analog path and the driver's buffering. It's small but real, and it
must be accounted for if you want input and output to line up (monitoring,
round-trip measurement, delay compensation).

> **→ in aiudio:** each backend reports `latencyFrames()` — assembled from the
> device's reported latency + **safety offset** (which include the converter and
> driver delays; see `coreaudio_input_backend.cpp`). That figure feeds the
> pipeline's delay-compensation story (PDC, G9) and the duplex monitor's
> round-trip latency in [`docs/cookbooks/81`](../cookbooks/81-pipeline-usage-patterns.md) §Pattern 3. The
> converter is part of *why* `latency_frames` is nonzero even for a trivial graph.

---

## 8. Where signals actually enter and leave aiudio

Not every source crosses an ADC, and not every sink a DAC — a useful distinction
the recorder patterns rely on:

| Path | Crosses a converter? | Notes |
|---|---|---|
| **Microphone / line in** | **ADC** (analog → PCM) | the classic capture; `InputBackend` |
| **System / per-app audio (tap)** | **no** — intercepted **before the DAC** | already-digital output of other apps, grabbed pre-conversion; `TapBackend` |
| **WAV / file in** | **no** (frozen) | PCM the ADC produced earlier, stored; `WavReader` |
| **Speakers / line out** | **DAC** (PCM → analog) | the classic playback; `DeviceBackend` |
| **WAV / file out / recorder** | **no** | PCM saved before any DAC; `WavWriter` / `WavRecorder` |

So the signed **`aiudio-recorder`** (docs/cookbooks/81 Pattern 7) mixes a **post-ADC** mic
with a **pre-DAC** system tap — two signals of totally different physical origin —
into one float32 timeline, and writes PCM **without ever touching a DAC**. The
converters bookend the *live* paths; the *file/tap* paths sidestep them.

> **→ in aiudio:** because the tap is pre-DAC, it also can't feed back through the
> DAC — one reason the recorder is playback-free (docs/cookbooks/81 Pattern 7). And because
> a file is frozen PCM, offline rendering (docs/cookbooks/81 Pattern 1) involves **no
> converter and no clock** at all — it's pure arithmetic, which is why it runs
> faster than real time.

---

## 9. Aliasing, revisited — a rule for the graph

The ADC's anti-alias filter protects against aliasing *at capture*. But **DSP
inside the graph can create new frequencies above Nyquist** — nonlinear stages
(distortion/waveshaping, hard clipping), and generators (oscillators with sharp
edges: saw/square). Those harmonics have no anti-alias filter between them and the
DAC's discrete grid, so they **fold back** into the audible band as
inharmonic "digital nastiness."

The standard fix is **internal oversampling** around the nonlinear node (upsample
→ process → low-pass → downsample), so the harmonics land below the *raised*
Nyquist before decimation.

> **→ in aiudio:** relevant to `WaveshaperNode` and the band-limited-ness of
> `OscillatorNode` (docs/cookbooks/82 §6/§7). aiudio does **not** oversample nodes today
> (docs/theory/73 §10 / docs/pipeline/78) — a known refinement. It matters doubly for **Phase 1**:
> a differentiable/neural node that generates harmonics (docs/theory/20, `docs/pipeline/79`) must
> respect Nyquist or train against aliased targets. (Note: this internal
> oversampling is *unrelated* to the ΔΣ oversampling in §3/§4 and to aiudio's
> boundary `Resampler`/`ResamplingSource`, which convert between *nominal block
> rates* and absorb *drift* — three different uses of the word "oversample.")

---

## 10. End-to-end: the converters around the pipeline

```
                 ┌─────────────────── ADC (hardware) ───────────────────┐
 mic/line ──►────┤ anti-alias LPF → ΔΣ modulator (MHz,1-bit) → decimator ├──► float32 PCM
 (volts)         └───────────────────────────────────────────────────────┘        │  device crystal = Fs
                                                                                    ▼
   system audio (other apps) ─── tapped PRE-DAC (already digital) ───────────► float32 PCM
                                                                                    │
        ┌───────────── aiudio graph: RenderCallback.process(in,out,frames,time) ───┴──┐
        │  PLANAR float32, ±1.0 — the engine lingua franca (docs/theory/73)                   │
        │  clock = the master converter's crystal (ADR-0005); off-clock sources →      │
        │  ResamplingSource + drift servo (ADR-0015); limiter guards the DAC (§4)      │
        └───────────────────────────────┬───────────────────────────────┬────────────┘
                                         │ to a file/recorder            │ to the master output
                                         ▼ (no DAC — frozen PCM)         ▼
                                    WavWriter / WavRecorder     ┌──── DAC (hardware) ────┐
                                                                │ interpolate → ΔΣ → LPF ├──► speaker
                                                                └────────────────────────┘   (volts)
```

| Converter concept | Reaches aiudio as | Where |
|---|---|---|
| Sample clock `Fs` | `StreamConfig.sampleRate`; the master clock (ADR-0005) | backends, `TimeInfo` |
| Crystal drift between devices | per-source drift servo | `ResamplingSource`/`DriftCompensator` (ADR-0015) |
| Converter + driver latency | `latencyFrames()` → PDC (G9) | `coreaudio_*_backend.cpp` |
| Full-scale / 0 dBFS clip | float ±1.0 convention; only the DAC clamps | `audio_buffer.hpp`, a limiter node |
| Native bit depth ↔ float | HAL delivers float32 live; int16⇄float only at WAV | `conversions.hpp` (files only) |
| Pre-DAC digital tap | a source that never saw an ADC | `TapBackend` (docs/cookbooks/81 §7) |
| Nyquist / anti-imaging | don't generate >Nyquist in-graph (oversample TBD) | `WaveshaperNode`/`OscillatorNode` (docs/cookbooks/82) |

---

## 11. What aiudio does / doesn't do about converters

- **Trusts the HAL's float32.** aiudio does not model or emulate converters; it
  consumes/produces the float32 Core Audio provides (the ADC/DAC + driver own the
  bit-depth and layout conversion for live I/O).
- **Handles their clock, not their electronics.** The one converter property
  aiudio actively manages is the **clock**: it uses it as the master and
  reconciles other converters' drift (ADR-0015). Jitter, ENOB, filter design are
  the hardware's job.
- **Guards the DAC but doesn't dither it.** A limiter can protect the output;
  aiudio's own float→int16 (WAV only) does not dither yet (docs/theory/73 §10).
- **Does not oversample nonlinear nodes yet** (§9) — a known refinement (docs/pipeline/78).
- **Sidesteps converters entirely** for file/offline and tap paths (§8).

## 12. Glossary

- **ADC / DAC** — analog↔digital converter (in / out).
- **Sample-and-hold** — freezes the analog value at each clock tick for the quantizer.
- **Anti-aliasing filter** — LPF *before* the ADC; blocks >Nyquist so it can't fold back.
- **Anti-imaging / reconstruction filter** — LPF *after* the DAC; removes spectral images.
- **Sigma-delta (ΔΣ)** — oversampled, noise-shaped 1-/few-bit modulator; the basis of
  most modern audio converters.
- **Oversampling** — sampling far above `Fs` (converter-internal) to ease filtering and
  shape noise. *(Distinct from aiudio's boundary resampling and from node oversampling.)*
- **Decimation / interpolation** — digital down- / up-sampling filters inside a converter.
- **Zero-order hold** — hold each sample constant until the next (naïve reconstruction).
- **Jitter** — timing error in the sample clock; raises the noise floor.
- **ENOB** — effective number of bits; the *real* resolution after noise/jitter/distortion.
- **Clock drift** — two nominally-equal crystals running at slightly different real rates.
- **Nyquist frequency** — `Fs/2`; the highest representable frequency (docs/theory/73 §2).

## References
- Companion: [`docs/theory/73`](73-digital-audio-encoding.md) (the digital number formats),
  [`docs/pipeline/77`](../pipeline/77-combining-multiple-audio-io.md) (why many clocks are hard).
- Pipeline: [`docs/pipeline/71`](../pipeline/71-io-layer-milestones.md)/[`docs/pipeline/72`](../pipeline/72-m1-aiudio-io-reference.md)
  (I/O layer), [`docs/pipeline/76`](../pipeline/76-multi-source-io-roadmap.md) (drift/multi-source),
  [`docs/cookbooks/81`](../cookbooks/81-pipeline-usage-patterns.md) (topologies & latency),
  [`docs/cookbooks/82`](../cookbooks/82-node-usage-patterns.md) (limiter / oscillator / waveshaper).
- Decisions: ADR-0005 (swappable clock = the converter clock), ADR-0007 (Core Audio I/O),
  ADR-0008/0015 (per-source rings + cross-clock drift).

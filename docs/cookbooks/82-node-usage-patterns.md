# 82 — Node Usage Patterns (cookbook)

> **Last updated:** 2026-06-30 · **Scope:** how to *use the nodes* — typical DSP chains and
> idioms built from the graph's node library, explained with runnable **C++ and Python**.
> Grounded in the merged node library (**✓ Verified**). This is the "what to wire together"
> companion to the per-node catalog in [`docs/pipeline/80`](../pipeline/80-pipeline-capabilities.md) §4, the roadmap
> in [`docs/pipeline/78`](../pipeline/78-node-library-roadmap.md), and the *topology* cookbook in
> [`docs/cookbooks/81`](81-pipeline-usage-patterns.md) (clocks/sources/output). Here the clock is
> irrelevant — every example is shown as a plain `ex.process(block)`; drop the same graph into
> any backend from `docs/cookbooks/81` unchanged.

---

## Contents
- [0. Node fundamentals](#0-node-fundamentals)
- [1. Gain staging — trim, mute, invert, metering](#1-gain-staging)
- [2. Tone shaping with a biquad (+ live sweep)](#2-tone-shaping-with-a-biquad)
- [3. A channel EQ with the parametric EQ](#3-a-channel-eq-with-the-parametric-eq)
- [4. Compression and limiting](#4-compression-and-limiting)
- [5. Noise gate / downward expander](#5-noise-gate)
- [6. Saturation (waveshaper) + DC blocker](#6-saturation-waveshaper--dc-blocker)
- [7. A synth voice (generators → filter → gain)](#7-a-synth-voice)
- [8. Delay — insert vs send](#8-delay--insert-vs-send)
- [9. Mixing sources — Sum vs Mixer](#9-mixing-sources--sum-vs-mixer)
- [10. Parallel processing (fan-out → mix back)](#10-parallel-processing)
- [11. Stereo — pan, width, mono-check](#11-stereo--pan-width-mono-check)
- [12. Channel routing with the channel matrix](#12-channel-routing-with-the-channel-matrix)
- [13. Capstone — a full channel strip](#13-capstone--a-full-channel-strip)
- [Appendix — Parameter index quick reference](#appendix--parameter-index-quick-reference)
- [Appendix — Cross-references](#appendix--cross-references)

---

## 0. Node fundamentals

Six things that apply to *every* node — internalize these and the patterns below are just
combinations.

1. **Ports & edges.** A node has N input and M output ports. `connect(src, src_port, dst,
   dst_port)` wires an **output port → an input port**. An **output port may fan out** to many
   downstream inputs; **each *input* port takes exactly one edge** — to combine signals into one
   input, use a `Sum`/`Mixer` node (this is what makes parallel chains, §10, work).
2. **Three node roles.** *Generators* have 0 inputs (`Source`, `Oscillator`, `Noise`).
   *Processors* are 1→1 (most nodes). *Sinks* have 0 outputs (`Sink`). A few change **channel
   width** (`Pan` 1→2, `Upmix`/`Downmix`, `ChannelMatrix`) — allowed mid-graph (G8).
3. **Parameters are index-based, live, and RT-safe.** Change a value while the graph runs with
   `ex.set_param(node, index, value)` (C++ `exec.postParam(node, index, value)`); it's queued on a
   lock-free ring and applied at the next block (ADR-0010) — never mutate the graph from another
   thread. Convenience wrappers: `set_gain` (a `Gain`), `set_cutoff`/`set_q` (any `Biquad`). The
   per-node index map is in the [appendix](#appendix--parameter-index-quick-reference).
4. **Gain-like params are click-free.** Continuous params (gain, mix, cutoff, …) are internally
   **smoothed**, so automating them doesn't zipper — you can sweep them per block safely.
5. **Latency is explicit and compensated.** Nodes that add delay (a **lookahead** compressor)
   report `latencyFrames()`; the executor **delay-compensates parallel paths** (PDC, G9) so a
   dry path stays phase-aligned with a processed one. (The `Delay` node's *musical* delay is
   internal feedback — it reports **0** latency; only lookahead-style latency is compensated.)
6. **Reading levels.** A `Meter` node is a 1→1 pass-through that exposes the running mean-square;
   read it any time with `g.meter_mean_square(node)` (C++ `meterNode->meanSquare()`), e.g.
   `dbfs = 10*log10(ms)`.

Conventions below: `import aiudio as a` (Python), `namespace aiudio` (C++); `SR` = sample rate.
Each example builds a `Graph`, compiles a `GraphExecutor`, and runs one block via
`ex.process(...)` — the same graph runs live/offline unchanged (see `docs/cookbooks/81`).

---

## 1. Gain staging

**Trim a level, mute, flip polarity, and watch it on a meter.** `Gain` is the workhorse: a
positive value scales, `0.0` mutes, a negative value inverts polarity (useful for phase/null
tests). A `Meter` after it reports the level.

**Python**
```python
import math, numpy as np, aiudio as a
SR = 48000.0
g = a.Graph()
src = g.add_source()
trim = g.add_gain(0.5)        # -6 dB trim
m    = g.add_meter()          # level probe
snk  = g.add_sink()
g.connect(src, 0, trim, 0); g.connect(trim, 0, m, 0); g.connect(m, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

ex.process(np.full((1, 512), 0.8, np.float32))          # push a block
print("level:", 10*math.log10(max(g.meter_mean_square(m), 1e-12)), "dBFS")
ex.set_gain(trim, 0.0)        # live: mute (click-free)
ex.set_gain(trim, -1.0)       # live: invert polarity
```

**C++**
```cpp
graph::Graph g;
const auto src  = g.addNode(std::make_unique<graph::SourceNode>());
const auto trim = g.addNode(std::make_unique<graph::GainNode>(0.5f));
auto meterUp = std::make_unique<graph::MeterNode>();
auto* meter = meterUp.get();                              // keep a handle to read the level
const auto m = g.addNode(std::move(meterUp));
const auto snk = g.addNode(std::make_unique<graph::SinkNode>());
g.connect(src, 0, trim, 0); g.connect(trim, 0, m, 0); g.connect(m, 0, snk, 0);
graph::GraphExecutor exec; exec.compile(g, 1, SR, 512);
// … exec.process(...) …  then:
const float dbfs = 10.0f * std::log10(std::max(meter->meanSquare(), 1e-12f));
exec.postParam(trim, graph::GainNode::kGain, 0.0f);       // live mute
```

---

## 2. Tone shaping with a biquad

**A single filter band — low-pass/high-pass tone control, or a peaking/shelf move — with a live
sweep.** `Biquad` is one RBJ-cookbook second-order section: choose the shape at build time
(`add_biquad_lowpass/highpass/peaking/lowshelf/highshelf`), then sweep `cutoff`/`q`/`gain_db`
live. Classic uses: a high-pass to remove rumble/DC-ish low end, a filter sweep as an effect.

**Python**
```python
g = a.Graph()
src = g.add_source()
hp  = g.add_biquad_highpass(80.0, 0.707, SR)     # remove sub-80 Hz rumble
lp  = g.add_biquad_lowpass(6000.0, 0.707, SR)    # tame harsh top
snk = g.add_sink()
g.connect(src, 0, hp, 0); g.connect(hp, 0, lp, 0); g.connect(lp, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

for hz in (6000, 3000, 1200, 600):               # a live low-pass sweep
    ex.set_cutoff(lp, float(hz)); ex.set_q(lp, 0.8)
    ex.process(block)
```

**C++**
```cpp
auto hpUp = std::make_unique<graph::BiquadNode>(/*maxChannels*/ 1);
hpUp->setHighpass(80.0, 0.707, SR);
const auto hp = g.addNode(std::move(hpUp));
auto lpUp = std::make_unique<graph::BiquadNode>(1);
lpUp->setLowpass(6000.0, 0.707, SR);
const auto lp = g.addNode(std::move(lpUp));
// connect src→hp→lp→snk, compile … then live:
exec.postParam(lp, graph::BiquadNode::kCutoffHz, 1200.0f);   // == exec set_cutoff
```

> A peaking/shelf band adds a **gain** param (index 2): `add_biquad_peaking(freq, q, gain_db, sr)`
> in Python, `setPeaking(freq, q, gainDb, sr)` in C++.

---

## 3. A channel EQ with the parametric EQ

**Several EQ bands in one node.** `ParametricEq` cascades biquad sections — build it from a list
of `(type, freq, q, gain_db)` bands (`type` ∈ `peaking|lowshelf|highshelf|lowpass|highpass`). A
typical vocal EQ: high-pass out the rumble, dip boxy low-mids, boost presence, add air. Each
band's params live at `band*3 + {0:freq, 1:q, 2:gain_db}`.

**Python**
```python
g = a.Graph()
src = g.add_source()
eq = g.add_parametric_eq([
    ("highpass",  90.0, 0.707,  0.0),   # band 0 — low cut
    ("peaking",  350.0, 1.2,   -3.0),   # band 1 — tame boxiness
    ("peaking", 4000.0, 1.0,   +3.5),   # band 2 — presence
    ("highshelf", 12000.0, 0.7, +2.0),  # band 3 — air
], sample_rate=SR)
snk = g.add_sink()
g.connect(src, 0, eq, 0); g.connect(eq, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

ex.set_param(eq, 2*3 + 2, +5.0)        # live: push band-2 (presence) gain to +5 dB
ex.set_param(eq, 2*3 + 0, 5000.0)      # live: move its centre to 5 kHz
```

**C++**
```cpp
using T = graph::BiquadNode::Type;
std::vector<graph::ParametricEqNode::Band> bands = {
    {T::Highpass,     90.0, 0.707,  0.0},
    {T::Peaking,     350.0, 1.2,   -3.0},
    {T::Peaking,    4000.0, 1.0,   +3.5},
    {T::HighShelf, 12000.0, 0.7,   +2.0},
};
const auto eq = g.addNode(std::make_unique<graph::ParametricEqNode>(bands, SR));
// connect, compile … then live:
exec.postParam(eq, /*band 2, gain*/ 2*3 + 2, +5.0f);
```

---

## 4. Compression and limiting

**Control dynamics.** One `Compressor` node covers gentle leveling → brick-wall limiting via its
params: `threshold_db`, `ratio`, `attack_ms`, `release_ms`, and (index 4) `makeup_db`. A
**lookahead** (frames, set at build) lets it catch transients before they happen — that adds
**latency**, which the executor reports and delay-compensates (§0.5, §10).

**Python** — a vocal compressor, then reconfigured as a limiter:
```python
g = a.Graph()
src = g.add_source()
comp = g.add_compressor(threshold_db=-18.0, ratio=3.0, attack_ms=5.0, release_ms=90.0)
snk = g.add_sink()
g.connect(src, 0, comp, 0); g.connect(comp, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

ex.set_param(comp, 4, +4.0)            # +4 dB makeup gain (index 4)
# live "make it a limiter": high ratio, fast attack, low threshold
ex.set_param(comp, 1, 20.0); ex.set_param(comp, 2, 1.0); ex.set_param(comp, 0, -1.0)

# A true limiter with lookahead (build-time) — reports latency for PDC:
lim = g.add_compressor(threshold_db=-1.0, ratio=20.0, attack_ms=1.0,
                       release_ms=60.0, lookahead_frames=64)
```

**C++**
```cpp
const auto comp = g.addNode(std::make_unique<graph::CompressorNode>(
    /*thresholdDb*/ -18.0f, /*ratio*/ 3.0f, /*attackMs*/ 5.0f, /*releaseMs*/ 90.0f));
// connect, compile … then live:
exec.postParam(comp, 4, +4.0f);                        // makeup gain
// limiter with 64-frame lookahead (adds compensated latency, G9):
const auto lim = g.addNode(std::make_unique<graph::CompressorNode>(
    -1.0f, 20.0f, 1.0f, 60.0f, /*lookaheadFrames*/ 64));
```

---

## 5. Noise gate

**Silence what's below a threshold** — clean up hiss/bleed between phrases, or tighten a noisy
signal. `Gate` is a downward expander: below `threshold_db` it attenuates by `range_db` (a large
negative value ≈ a hard gate); `attack_ms`/`release_ms` set how fast it opens/closes.

**Python**
```python
g = a.Graph()
src = g.add_source()
gate = g.add_gate(threshold_db=-45.0, attack_ms=1.0, release_ms=120.0, range_db=-80.0)
snk = g.add_sink()
g.connect(src, 0, gate, 0); g.connect(gate, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

ex.set_param(gate, 0, -40.0)   # raise the threshold (index 0) — gate more aggressively
ex.set_param(gate, 3, -30.0)   # softer expander (only -30 dB below threshold, not a hard cut)
```

**C++**
```cpp
const auto gate = g.addNode(std::make_unique<graph::GateNode>(
    /*thresholdDb*/ -45.0f, /*attackMs*/ 1.0f, /*releaseMs*/ 120.0f, /*rangeDb*/ -80.0f));
// connect, compile … then:  exec.postParam(gate, 0, -40.0f);
```

---

## 6. Saturation (waveshaper) + DC blocker

**Add harmonics / glue / grit.** `Waveshaper` applies a nonlinear curve: `tanh` (smooth, tube-ish),
`softclip` (rounded limiting), `hardclip` (aggressive). `drive` (index 0) pushes the input into
the curve; `mix` (index 1) blends dry↔wet for **parallel saturation** (keep the transients clean,
warm the body). Nonlinear/asymmetric shaping can introduce a DC offset — **follow it with a
`DcBlocker`**.

**Python**
```python
g = a.Graph()
src = g.add_source()
sat = g.add_waveshaper("tanh", drive=2.0, mix=0.5)   # 50% wet parallel drive
dc  = g.add_dc_blocker(20.0)                          # remove any offset the curve adds
snk = g.add_sink()
g.connect(src, 0, sat, 0); g.connect(sat, 0, dc, 0); g.connect(dc, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

ex.set_param(sat, 0, 4.0)     # more drive (index 0)
ex.set_param(sat, 1, 0.3)     # back off to 30% wet (index 1)
```

**C++**
```cpp
const auto sat = g.addNode(std::make_unique<graph::WaveshaperNode>(
    graph::WaveshaperNode::Shape::Tanh, /*drive*/ 2.0f, /*mix*/ 0.5f));
const auto dc  = g.addNode(std::make_unique<graph::DcBlockerNode>(/*cornerHz*/ 20.0));
// connect src→sat→dc→snk, compile … then:  exec.postParam(sat, 0, 4.0f);
```

---

## 7. A synth voice

**Generators need no input** (0→1). Build a subtractive voice: a `saw` oscillator → low-pass
filter → gain; add filtered `noise` for breath/percussion. Combine the oscillator and noise with
a `Sum` (or `Mixer`) since each feeds a separate branch.

**Python**
```python
g = a.Graph()
osc  = g.add_oscillator("saw", freq=110.0, amplitude=0.6)   # 0 in → 1 out
tone = g.add_biquad_lowpass(1200.0, 1.5, SR)                # resonant filter
nz   = g.add_noise("white", amplitude=0.15)
hpn  = g.add_biquad_highpass(6000.0, 0.7, SR)               # noise → "air"
mix  = g.add_sum(2)
amp  = g.add_gain(0.8)
snk  = g.add_sink()
g.connect(osc, 0, tone, 0); g.connect(tone, 0, mix, 0)
g.connect(nz, 0, hpn, 0);   g.connect(hpn, 0, mix, 1)
g.connect(mix, 0, amp, 0);  g.connect(amp, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

out = ex.process(np.zeros((1, 512), np.float32))   # generators ignore input; still pass a block
ex.set_param(osc, 0, 220.0)                         # live pitch (index 0 = freq)
ex.set_cutoff(tone, 2500.0)                         # live filter open
```

**C++**
```cpp
const auto osc = g.addNode(std::make_unique<graph::OscillatorNode>(
    graph::OscillatorNode::Waveform::Saw, /*freq*/ 110.0, /*amp*/ 0.6f));
auto toneUp = std::make_unique<graph::BiquadNode>(1); toneUp->setLowpass(1200.0, 1.5, SR);
const auto tone = g.addNode(std::move(toneUp));
const auto nz  = g.addNode(std::make_unique<graph::NoiseNode>(
    graph::NoiseNode::Color::White, /*amp*/ 0.15f, /*maxCh*/ 1));
const auto mix = g.addNode(std::make_unique<graph::SumNode>(2));
// connect osc→tone→mix:0, nz→…→mix:1, mix→gain→sink, compile … then:
exec.postParam(osc, graph::OscillatorNode::kFreq, 220.0f);   // (or index 0)
```

---

## 8. Delay — insert vs send

`Delay` is a feedback delay line with a wet/dry `mix` (index 2), `feedback` (index 1), and
`delay_frames` (index 0). Two idioms:

- **Insert** (in the main path): set `mix` to taste — the node blends dry+wet itself. Simplest.
- **Send** (parallel): keep the dry at 100% on one branch, run a *100%-wet* delay on a parallel
  branch, and blend with a `Mixer` — the classic aux-send routing (see also §10).

**Python** — insert echo, then a send:
```python
# Insert: one node does dry+wet.
g = a.Graph()
src = g.add_source()
dly = g.add_delay(max_seconds=1.0, delay_frames=int(0.25*SR), feedback=0.35, mix=0.3)
snk = g.add_sink()
g.connect(src, 0, dly, 0); g.connect(dly, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)
ex.set_param(dly, 0, 0.125*SR)   # 1/8-note delay (index 0 = delay_frames)
ex.set_param(dly, 1, 0.5)        # more feedback (index 1)

# Send: dry stays 100%; a 100%-wet delay is mixed in parallel.
g2 = a.Graph()
s2   = g2.add_source()
send = g2.add_delay(max_seconds=1.0, delay_frames=int(0.375*SR), feedback=0.4, mix=1.0)  # fully wet
bus  = g2.add_mixer(2, 1.0)      # input 0 = dry, input 1 = echo return
o2   = g2.add_sink()
g2.connect(s2, 0, bus, 0)        # dry (fan-out of the source)
g2.connect(s2, 0, send, 0); g2.connect(send, 0, bus, 1)  # wet return
g2.connect(bus, 0, o2, 0)
```

**C++** (the insert)
```cpp
const auto dly = g.addNode(std::make_unique<graph::DelayNode>(
    /*maxSeconds*/ 1.0, /*delayFrames*/ (std::uint32_t)(0.25*SR), /*feedback*/ 0.35f, /*mix*/ 0.3f));
// connect src→dly→snk, compile … then:  exec.postParam(dly, 0, 0.125f*SR);  // 1/8 note
```

---

## 9. Mixing sources — Sum vs Mixer

Combining several signals into one input port needs a combiner node (§0.1):

- **`Sum`** — unity add of N inputs (no per-input control). Use when levels are already set.
- **`Mixer`** — N inputs each with its **own gain** (`param index i = gain of input i`), plus an
  overall gain at build. A mini console; ride levels live.

**Python** — a 3-input mixer with live level rides:
```python
g = a.Graph()
i0, i1, i2 = g.add_source(0), g.add_source(1), g.add_source(2)
mixer = g.add_mixer(num_inputs=3, gain=1.0)
snk = g.add_sink(0)
g.connect(i0, 0, mixer, 0); g.connect(i1, 0, mixer, 1); g.connect(i2, 0, mixer, 2)
g.connect(mixer, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

out = ex.process_multi([blk0, blk1, blk2], num_outputs=1)[0]   # 3 input streams → 1 out
ex.set_param(mixer, 0, 1.0)     # input 0 at unity
ex.set_param(mixer, 1, 0.5)     # input 1 down 6 dB
ex.set_param(mixer, 2, 0.0)     # input 2 muted
```

**C++**
```cpp
const auto mixer = g.addNode(std::make_unique<graph::MixerNode>(/*numInputs*/ 3, /*gain*/ 1.0f));
// connect i0→mixer:0, i1→mixer:1, i2→mixer:2, mixer→snk, compile … then:
exec.postParam(mixer, 1, 0.5f);   // input 1 gain
```

---

## 10. Parallel processing

**Split one signal, process the copies differently, mix them back.** Because an output port can
**fan out** (§0.1), you route the source to two branches and recombine with a `Mixer`. The classic
case is **parallel ("New York") compression**: blend the dry signal with a heavily-compressed
copy. The executor's **PDC** (§0.5) keeps the two branches phase-aligned even if one adds latency.

```
          ┌───────────────────────────► mixer:0 (dry)
 source ──┤
          └─► compressor (hard) ───────► mixer:1 (crushed)   →  sink
```

**Python**
```python
g = a.Graph()
src = g.add_source()
crush = g.add_compressor(threshold_db=-30.0, ratio=10.0, attack_ms=2.0, release_ms=120.0)
bus = g.add_mixer(2, 1.0)          # 0 = dry, 1 = parallel-compressed
snk = g.add_sink()
g.connect(src, 0, bus, 0)          # dry branch (fan-out #1)
g.connect(src, 0, crush, 0); g.connect(crush, 0, bus, 1)   # processed branch (fan-out #2)
g.connect(bus, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)
ex.set_param(bus, 1, 0.4)          # blend in 40% of the crushed copy
```

**C++**
```cpp
const auto crush = g.addNode(std::make_unique<graph::CompressorNode>(-30.0f, 10.0f, 2.0f, 120.0f));
const auto bus   = g.addNode(std::make_unique<graph::MixerNode>(2, 1.0f));
g.connect(src, 0, bus, 0);                                   // dry
g.connect(src, 0, crush, 0); g.connect(crush, 0, bus, 1);    // processed
g.connect(bus, 0, snk, 0);
// compile … then:  exec.postParam(bus, 1, 0.4f);            // parallel blend
```

---

## 11. Stereo — pan, width, mono-check

- **`Pan`** places a **mono** source in the stereo field (equal-power, 1→**2** ch; index 0 =
  pan −1…+1).
- **`StereoWidth`** narrows/widens an existing **stereo** signal via mid/side (index 0: `0`=mono,
  `1`=unchanged, `>1`=wider).
- **`Downmix`** folds stereo→mono — handy to **check mono compatibility** after widening.

**Python**
```python
g = a.Graph()
src   = g.add_source()             # mono
pan   = g.add_pan(-0.3)            # slightly left → stereo
width = g.add_stereo_width(1.4)    # widen
snk   = g.add_sink()               # stereo out
g.connect(src, 0, pan, 0); g.connect(pan, 0, width, 0); g.connect(width, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=2, sample_rate=SR, max_block=512)  # 2ch graph

# NOTE: the numpy process() returns an array with the SAME channel count as the input, so to
# capture the widened *stereo* output, feed a 2-channel block (the mono source reads channel 0).
# In a live/offline backend the output width is the device/file's — this is a numpy-bridge detail.
out = ex.process(np.tile(mono_block, (2, 1)))    # (2, N) in → (2, N) out
ex.set_param(pan, 0, 0.5)          # ride the pan to the right
ex.set_param(width, 0, 0.0)        # collapse to mono (A/B the image)
```

**C++**
```cpp
const auto pan   = g.addNode(std::make_unique<graph::PanNode>(/*pan*/ -0.3f));       // 1 → 2 ch
const auto width = g.addNode(std::make_unique<graph::StereoWidthNode>(/*width*/ 1.4f));
// connect src→pan→width→snk, compile at channels=2 … then:  exec.postParam(pan, 0, 0.5f);
```

> **Width note (G8):** channel width changes *inside* the graph — `Pan` outputs 2 ch even though
> its input is 1 ch. Compile the executor at the **widest** width the graph uses (here `channels=2`).

---

## 12. Channel routing with the channel matrix

**`ChannelMatrix`** is a general in→out routing/mixing matrix. Cell `(out, in)` lives at param
index `out * in_channels + in`; set it to the gain from input channel `in` to output channel
`out`. Use it to swap channels, fold to mono, or build custom routings.

**Python** — swap L/R, and (separately) sum stereo → mono:
```python
g = a.Graph()
src = g.add_source()
swap = g.add_channel_matrix(in_channels=2, out_channels=2)
snk = g.add_sink()
g.connect(src, 0, swap, 0); g.connect(swap, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=2, sample_rate=SR, max_block=512)

# swap: out0 ← in1, out1 ← in0  (index = out*in_channels + in)
ex.set_param(swap, 0*2 + 0, 0.0); ex.set_param(swap, 0*2 + 1, 1.0)   # out0 = in1
ex.set_param(swap, 1*2 + 0, 1.0); ex.set_param(swap, 1*2 + 1, 0.0)   # out1 = in0
# a 2→1 fold-to-mono matrix instead:  add_channel_matrix(2, 1) with cells (0,0)=(0,1)=0.5
```

**C++**
```cpp
const auto swap = g.addNode(std::make_unique<graph::ChannelMatrixNode>(/*inCh*/ 2, /*outCh*/ 2));
// compile at channels=2 … then set the four cells:
exec.postParam(swap, 0*2 + 1, 1.0f);   // out0 ← in1
exec.postParam(swap, 1*2 + 0, 1.0f);   // out1 ← in0
```

---

## 13. Capstone — a full channel strip

The canonical vocal/instrument strip, top to bottom, combining the patterns above:
**high-pass → parametric EQ → compressor → gentle saturation → output gain → meter**. Build once,
mix live.

```
source → HPF(80) → parametric EQ → compressor → waveshaper(tanh, subtle) → gain → meter → sink
```

**Python**
```python
import math, numpy as np, aiudio as a
SR = 48000.0
g = a.Graph()
src  = g.add_source()
hp   = g.add_biquad_highpass(80.0, 0.707, SR)
eq   = g.add_parametric_eq([("peaking", 300.0, 1.2, -2.5),
                            ("peaking", 4000.0, 1.0, +3.0),
                            ("highshelf", 12000.0, 0.7, +1.5)], sample_rate=SR)
comp = g.add_compressor(threshold_db=-18.0, ratio=3.0, attack_ms=6.0, release_ms=90.0)
sat  = g.add_waveshaper("tanh", drive=1.3, mix=0.25)   # a touch of warmth
out  = g.add_gain(1.0)
mtr  = g.add_meter()
snk  = g.add_sink()
for a_, b_ in [(src,hp),(hp,eq),(eq,comp),(comp,sat),(sat,out),(out,mtr),(mtr,snk)]:
    g.connect(a_, 0, b_, 0)
assert g.validate()[0]
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)

y = ex.process(np.random.default_rng(0).standard_normal((1, 512)).astype(np.float32) * 0.3)
print("strip out level:", 10*math.log10(max(g.meter_mean_square(mtr), 1e-12)), "dBFS")

# live moves:
ex.set_param(comp, 4, +3.0)          # +3 dB makeup
ex.set_param(eq, 1*3 + 2, +5.0)      # push the presence band
ex.set_cutoff(hp, 120.0)             # steeper low cut
ex.set_gain(out, 0.7)                # trim the output
```

**C++** (build; live moves identical via `exec.postParam(...)`)
```cpp
using T = graph::BiquadNode::Type;
graph::Graph g;
const auto src = g.addNode(std::make_unique<graph::SourceNode>());
auto hpUp = std::make_unique<graph::BiquadNode>(1); hpUp->setHighpass(80.0, 0.707, SR);
const auto hp = g.addNode(std::move(hpUp));
std::vector<graph::ParametricEqNode::Band> bands = {
    {T::Peaking, 300.0, 1.2, -2.5}, {T::Peaking, 4000.0, 1.0, +3.0},
    {T::HighShelf, 12000.0, 0.7, +1.5}};
const auto eq   = g.addNode(std::make_unique<graph::ParametricEqNode>(bands, SR));
const auto comp = g.addNode(std::make_unique<graph::CompressorNode>(-18.0f, 3.0f, 6.0f, 90.0f));
const auto sat  = g.addNode(std::make_unique<graph::WaveshaperNode>(
                      graph::WaveshaperNode::Shape::Tanh, 1.3f, 0.25f));
const auto out  = g.addNode(std::make_unique<graph::GainNode>(1.0f));
auto mtrUp = std::make_unique<graph::MeterNode>(); auto* meter = mtrUp.get();
const auto mtr = g.addNode(std::move(mtrUp));
const auto snk = g.addNode(std::make_unique<graph::SinkNode>());
g.connect(src,0,hp,0); g.connect(hp,0,eq,0); g.connect(eq,0,comp,0);
g.connect(comp,0,sat,0); g.connect(sat,0,out,0); g.connect(out,0,mtr,0); g.connect(mtr,0,snk,0);
graph::GraphExecutor exec; exec.compile(g, 1, SR, 512);
// … exec.process(...) …  const float ms = meter->meanSquare();
exec.postParam(eq, 1*3 + 2, +5.0f);   // presence band gain
```

---

## Appendix — Parameter index quick reference

Live control: `ex.set_param(node, index, value)` (C++ `exec.postParam(node, index, value)`).
Convenience: `set_gain` (Gain), `set_cutoff`/`set_q` (Biquad).

| Node | Ports | Param indices |
|---|---|---|
| **Gain** | 1→1 | 0 = gain (`kGain`) |
| **Sum** | N→1 | — |
| **Mixer** | N→1 | *i* = gain of input *i* |
| **Meter** | 1→1 | — (read `g.meter_mean_square(node)`) |
| **Biquad** LP/HP | 1→1 | 0 = cutoff Hz (`kCutoffHz`), 1 = Q (`kQ`) |
| **Biquad** peaking/shelf | 1→1 | 0 = freq, 1 = Q, 2 = gain dB (`kGainDb`) |
| **Parametric EQ** | 1→1 | `band*3 + {0:freq, 1:Q, 2:gain_db}` |
| **Compressor** | 1→1 | 0 = threshold_db, 1 = ratio, 2 = attack_ms, 3 = release_ms, 4 = makeup_db |
| **Gate** | 1→1 | 0 = threshold_db, 1 = attack_ms, 2 = release_ms, 3 = range_db |
| **Delay** | 1→1 | 0 = delay_frames, 1 = feedback, 2 = mix |
| **Waveshaper** | 1→1 | 0 = drive, 1 = mix |
| **Oscillator** | 0→1 | 0 = freq, 1 = amplitude |
| **Noise** | 0→1 | 0 = amplitude |
| **Pan** | 1→2 | 0 = pan (−1…+1) |
| **Stereo width** | 1→1 (2ch) | 0 = width |
| **Channel matrix** | 1→1 (width change) | `out*in_channels + in` = cell gain |
| **DC blocker / Downmix / Upmix / Source / Sink** | — | (no live params) |

## Appendix — Cross-references

- **Per-node catalog + factories:** [`docs/pipeline/80`](../pipeline/80-pipeline-capabilities.md) §4.
- **Node-library roadmap (tiers, planned nodes):** [`docs/pipeline/78`](../pipeline/78-node-library-roadmap.md).
- **Topology patterns (clocks/sources/output — where to run these graphs):** [`docs/cookbooks/81`](81-pipeline-usage-patterns.md).
- **Node contract, executor, live-control queue:** ADR-0009 (graph IR), ADR-0010 (control plane),
  [`docs/theory/50`](../theory/50-architecture-patterns.md), [`docs/pipeline/74`](../pipeline/74-graph-spine-milestones.md).
- **Runnable examples:** `examples/python/ex_graph_numpy.py`, `ex_live_control.py`;
  `examples/cpp/ex_build_graph.cpp`, `ex_run_graph_offline.cpp`.

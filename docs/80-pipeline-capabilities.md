# 80 — Pipeline Capabilities & Usage Guide (Phase 0)

> **Last updated:** 2026-06-30 · **Scope:** everything the aiudio pipeline can do **today**
> (end of Phase 0), with runnable C++ and Python examples. Grounded in the merged code
> (**✓ Verified**). For *why* it's built this way see the ADRs (`adr/`); for what's *next* see
> `docs/76` (multi-source), `docs/78` (node library), and the README roadmap.

---

## Contents
1. [The model in one screen](#1-the-model-in-one-screen)
2. [Core concepts](#2-core-concepts)
3. [Quick start](#3-quick-start)
4. [The node library](#4-the-node-library)
5. [Running a graph (1-stream & multi-stream)](#5-running-a-graph)
6. [The live control plane](#6-the-live-control-plane)
7. [Editing & inspecting a graph](#7-editing--inspecting-a-graph)
8. [Backends — offline, live device, mock](#8-backends)
9. [Multi-source & multi-clock](#9-multi-source--multi-clock)
10. [Boundary DSP utilities](#10-boundary-dsp-utilities)
11. [End-to-end recipes](#11-end-to-end-recipes)
12. [What Phase 0 does *not* do yet](#12-what-phase-0-does-not-do-yet)

---

## 1. The model in one screen

- **C++ owns the real-time core**, Python is a **control/research frontend** (ADR-0002). Python
  builds, runs, edits, and monitors graphs; it **never touches the audio thread** (ADR-0004).
- **One typed IR, many backends** (ADR-0009/0005): a `Graph` compiles to a static schedule that
  the same `GraphExecutor` runs under *any* clock — an offline file pump, a live Core Audio
  device, or a manual/mock tick — bit-for-bit identically.
- **Audio is planar float32**, processed in **blocks** of `frames`. Everything crossing a thread
  boundary is a **lock-free SPSC ring** or an **atomic** (ADR-0004/0008).
- **DSP and (future) neural nodes are peers** under one **node contract** (`process()` +
  metadata: `realtime_capable`, `latencyFrames()`, channel layout).

```
 Python (build / edit / monitor)  ──nanobind──┐  control-rate edits (lock-free queue)
   Graph  ──compile──►  GraphExecutor          │
                          │                     ▼
 C++ RT core:   backend (device / file / mock) ─drives─► executor.process(in,out,frames,time)
                          └ inline nodes (no alloc, no locks, no Python on the audio thread)
```

---

## 2. Core concepts

| Concept | What it is |
|---|---|
| **`AudioBuffer`** | A non-owning view of planar float32: `channels[ch][frame]`, with `numChannels`/`numFrames`. The buffer type at every boundary. |
| **`Graph`** | Nodes + directed edges (`src:outPort → dst:inPort`). A **DAG**; each input port has **at most one** incoming edge (mix with a `SumNode`/`MixerNode`). Built/edited off-thread. |
| **`Node`** | A processor with N input + M output ports. Implements `process()` (RT-safe), reports `numInputs/numOutputs`, channel layout, and `latencyFrames()`. DSP and neural are the same kind. |
| **`GraphExecutor`** | Compiles a `Graph` into a topological schedule with pre-allocated per-port buffers, then runs it as a `RenderCallback`. Live-editable (atomic schedule swap, RCU). |
| **Block / frames / channels** | `process()` handles `frames` samples per channel per call, up to the compiled `max_block`. Channel width can change *inside* the graph (per-port, G8). |
| **Control plane** | Parameter edits are **enqueued** on a lock-free queue and applied by the audio thread at the next block — never a lock, never a Python call on the audio thread (ADR-0010). |

Python = the **bound subset** of the C++ surface; there is no Python-only logic (ADR-0002).

---

## 3. Quick start

### Build & install

```bash
# C++ core + tests
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure

# Python package (builds the C++ core via scikit-build-core)
python -m pip install .          # then:  import aiudio
```

### Minimal graph — Python

```python
import numpy as np, aiudio as a

g = a.Graph()
src  = g.add_source()                       # 0 in, 1 out — the external input
gain = g.add_gain(0.5)
sink = g.add_sink()                         # 1 in, 0 out — the external output
g.connect(src, 0, gain, 0)
g.connect(gain, 0, sink, 0)
ok, err = g.validate(); assert ok, err

ex = a.GraphExecutor()
ex.compile(g, channels=1, sample_rate=48000.0, max_block=512)
out = ex.process(np.ones((1, 256), np.float32))   # (channels, frames) in → out
print(out[0, 0])                                   # 0.5
```

### Minimal graph — C++

```cpp
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sink_node.hpp"
using namespace aiudio::graph;

Graph g;
const NodeId src  = g.addNode(std::make_unique<SourceNode>());
const NodeId gain = g.addNode(std::make_unique<GainNode>(0.5f));
const NodeId sink = g.addNode(std::make_unique<SinkNode>());
g.connect(src, 0, gain, 0);
g.connect(gain, 0, sink, 0);

GraphExecutor exec;
exec.compile(g, /*channels*/ 1, /*sampleRate*/ 48000.0, /*maxBlock*/ 512);

float in[256], out[256];
for (float& v : in) v = 1.0f;
float* ic[1] = {in}; float* oc[1] = {out};
aiudio::io::AudioBuffer ib{ic, 1, 256}, ob{oc, 1, 256};
exec.process(ib, ob, 256, aiudio::io::TimeInfo{});   // out[0] == 0.5f
```

The executor **is** a `RenderCallback`, so the very same `exec` is later handed to a file or
device backend to run live (§8) — no graph changes.

---

## 4. The node library

Every node is added with `Graph.add_*` (Python) or `addNode(std::make_unique<…>)` (C++). Live
control uses `GraphExecutor.set_param(node, index, value)` with the indices below; continuous
gain-like params are **click-free** (smoothed). Full catalog + roadmap: `docs/78`.

| Node | Python factory | Ports | Live params (index) |
|---|---|---|---|
| **Source / Sink** | `add_source(stream=0)` / `add_sink(stream=0)` | 0→1 / 1→0 | — |
| **Gain** (also trim/mute/invert) | `add_gain(gain)` | 1→1 | 0=gain |
| **Sum** (mixer, no gains) | `add_sum(num_inputs)` | N→1 | — |
| **Mixer** (per-input gains) | `add_mixer(num_inputs, gain)` | N→1 | i=gain of input i |
| **Meter** (level telemetry) | `add_meter()` | 1→1 | — (read `meter_mean_square`) |
| **Biquad** LP/HP | `add_biquad_lowpass/​highpass(freq,q,sr)` | 1→1 | 0=freq 1=q |
| **Biquad** peaking/shelf | `add_biquad_peaking/​lowshelf/​highshelf(freq,q,gain_db,sr)` | 1→1 | 0=freq 1=q 2=gain_db |
| **Biquad** raw | `add_biquad_coeffs(b0,b1,b2,a1,a2)` | 1→1 | — |
| **Parametric EQ** | `add_parametric_eq([(type,freq,q,gain_db),…], sr)` | 1→1 | band·3 + {0:freq,1:q,2:gain} |
| **Compressor / limiter** | `add_compressor(threshold_db,ratio,attack_ms,release_ms,lookahead_frames,max_ch)` | 1→1 | 0=thr 1=ratio 2=atk 3=rel 4=makeup |
| **Gate / expander** | `add_gate(threshold_db,attack_ms,release_ms,range_db)` | 1→1 | 0=thr 1=atk 2=rel 3=range |
| **Delay** (feedback) | `add_delay(max_seconds,delay_frames,feedback,mix,max_ch)` | 1→1 | 0=delay 1=feedback 2=mix |
| **Waveshaper** (saturation) | `add_waveshaper(shape,drive,mix)` (`tanh`/`softclip`/`hardclip`) | 1→1 | 0=drive 1=mix |
| **Oscillator** | `add_oscillator(waveform,freq,amplitude)` (`sine`/`saw`/`square`/`triangle`) | 0→1 | 0=freq 1=amplitude |
| **Noise** | `add_noise(color,amplitude,max_ch)` (`white`/`pink`) | 0→1 | 0=amplitude |
| **Pan** (equal-power) | `add_pan(pan)` (mono→stereo) | 1→1(2ch) | 0=pan |
| **Stereo width** (M/S) | `add_stereo_width(width)` | 1→1 | 0=width |
| **Downmix / Upmix** | `add_downmix()` / `add_upmix(channels)` | 1→1 (width change) | — |
| **Channel matrix** (routing) | `add_channel_matrix(in_ch,out_ch)` | 1→1 (width change) | out·in_ch+in=gain |
| **DC blocker** | `add_dc_blocker(corner_hz,max_ch)` | 1→1 | — |
| **Latency** (test/model) | `add_latency(frames,max_ch)` | 1→1 | — (reports latency) |

C++ constructors mirror these (header-only, in `aiudio::graph`), e.g. `GainNode(0.5f)`,
`BiquadNode` + `setPeaking(freq,q,gainDb,sr)`, `CompressorNode(threshDb,ratio,atkMs,relMs,lookahead,maxCh)`,
`OscillatorNode(OscillatorNode::Waveform::Saw, 220.0, 0.6f)`, `ParametricEqNode({{BiquadNode::Type::LowShelf,200,0.7,6}}, sr)`.

---

## 5. Running a graph

### One stream (`process`)

`process((channels, frames)) → (channels, frames)`. The compiled `channels`/`max_block` bound
the block; larger blocks are silenced past `max_block` and counted as an xrun (telemetry).

### N inputs / M outputs (`process_multi`, G10)

Bind sources/sinks to **stream indices**; feed one array per input stream, get one per output.

```python
g = a.Graph()
a0, a1 = g.add_source(stream=0), g.add_source(stream=1)   # two input streams
mix    = g.add_sum(2)
out0   = g.add_sink(stream=0)
g.connect(a0, 0, mix, 0); g.connect(a1, 0, mix, 1); g.connect(mix, 0, out0, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=48000.0, max_block=128)

left  = np.full((1, 128), 0.3, np.float32)
right = np.full((1, 128), 0.7, np.float32)
[summed] = ex.process_multi([left, right])               # → list, one per output stream
print(summed[0, 0])                                      # 1.0
print(ex.input_streams, ex.output_streams, ex.latency_frames)
```

C++ multi-stream:
```cpp
io::AudioBuffer ins[2]  = {bufL, bufR};
io::AudioBuffer outs[1] = {bufOut};
exec.process(ins, /*numInputs*/ 2, outs, /*numOutputs*/ 1, frames, time);
```

Telemetry (both languages): `render_count`, `xrun_count`, `dropped_commands`, `latency_frames`,
`channels`, `input_streams`, `output_streams`, `compiled`.

---

## 6. The live control plane

Edits are enqueued on a lock-free queue and applied at the next block — safe to call **while a
device is running**. Returns `False`/`false` only if the queue is momentarily full.

```python
ex.set_gain(gain_node, 0.25)            # convenience for GainNode
ex.set_cutoff(eq_band, 5000.0)          # BiquadNode cutoff (Hz)
ex.set_q(eq_band, 1.5)                  # BiquadNode Q
ex.set_param(comp, 0, -24.0)            # generic: node, param index, value (compressor threshold)
print(ex.render_count, ex.xrun_count, ex.dropped_commands)
```

```cpp
exec.postParam(gainNode, GainNode::kGain, 0.25f);
exec.postParam(eqBand,   BiquadNode::kCutoffHz, 5000.0f);
exec.postParam(comp,     CompressorNode::kThresholdDb, -24.0f);
```

Continuous params (gain, drive, mix, pan, width, mixer/EQ gains) **ramp** to their target via a
one-pole smoother, so live moves don't click.

---

## 7. Editing & inspecting a graph

Read the IR back and edit it; **recompile** to apply. `remove_node` *tombstones* the slot so
existing `NodeId`s never shift.

```python
print(g.nodes())            # [(id, type_name, num_in, num_out), …] (live nodes)
print(g.edges())            # [(src, src_port, dst, dst_port), …]
print(g.node_type(eq))      # "BiquadNode"  (None if removed)

g.disconnect(gain, 0, sink, 0)   # remove one edge
g.remove_node(gain)              # tombstone + drop its edges (id stays valid, node_type→None)
g.connect(src, 0, sink, 0)       # rewire
ex.compile(g, channels=1, sample_rate=48000.0, max_block=512)   # apply
```

C++ has the same surface: `g.disconnect(...)`, `g.removeNode(id)`, `g.node(id)`, `g.edges()`,
`g.nodeCount()`, `g.liveNodeCount()`.

---

## 8. Backends

A backend is the **clock** that drives the executor (ADR-0005). All take a `RenderCallback*`
(the executor) in C++; in Python they take the executor object and **release the GIL** on
start/stop/open.

### Offline file render (cross-platform)

```python
ob = a.OfflineBackend("in.wav", "out.wav", a.WavFormat.Int16)   # reads in.wav's format
ex = a.GraphExecutor()
ex.compile(g, channels=ob.input_channels, sample_rate=ob.input_sample_rate, max_block=1024)
ob.open(ex, block_size=512)
ob.start()                                   # renders to completion, synchronously
print(ob.frames_rendered)
```

```cpp
io::OfflineBackend ob{"in.wav", "out.wav", io::WavFormat::Int16};
io::StreamConfig cfg; cfg.blockSize = 512;
ob.open(cfg, &exec);
ob.start();
```

### Direct WAV read/write (`WavReader` / `WavWriter`)

When you want to write processed blocks yourself (a render loop, a recorder) or read a WAV into
numpy without a third-party dependency, use `WavReader`/`WavWriter` directly. Both handle
canonical **PCM-16** and **32-bit-float** WAV with the planar `(channels, frames)` float32
convention. **These do blocking file I/O — the control/offline thread only, never the audio
thread (ADR-0004).** For a live recorder, drain a lock-free ring into a `WavWriter` on a writer
thread; do not call `write()` from a device IOProc.

```python
# Capture a graph's processed output to a WAV (off the audio thread).
with a.WavWriter("out.wav", channels=1, sample_rate=48000.0, format=a.WavFormat.Float32) as w:
    for _ in range(n_blocks):
        out = ex.process(block)          # C++ runs the graph
        w.write(np.ascontiguousarray(out))   # Python writes off-thread
# `with` finalizes the header on exit; a dropped writer is also finalized by its destructor (RAII).

r = a.WavReader("out.wav")               # read back into numpy
print(r.ok, r.channels, r.sample_rate, r.total_frames)
data = r.read(r.total_frames)            # (channels, frames) float32; short read => end of data
```

```cpp
io::WavWriter w{"out.wav", /*channels*/ 1, /*sampleRate*/ 48000.0, io::WavFormat::Float32};
w.write(planar, 1, frames);              // append a planar block
w.finalize();                            // or rely on the destructor (idempotent)
```

### Live device — macOS (Core Audio)

Output (`DeviceBackend`), mic input (`InputBackend`), full-duplex (`DuplexBackend`), and
system/per-app capture (`TapBackend`). Capture needs microphone TCC; taps need a signed binary
with `NSAudioCaptureUsageDescription` + audio-capture TCC.

```python
be = a.DeviceBackend()
for d in be.enumerate():                     # AudioDeviceInfo: id, name, in/out channels, …
    print(d.name, d.output_channels)
be.open(ex, channels=2, sample_rate=48000.0, block_size=512)   # routes RT blocks to the executor
be.start()                                   # real-time C++ audio thread
# … edit params live via ex.set_* …
be.stop()
print(be.running, be.latency_frames, be.xrun_count, be.disconnected)
be.set_disconnect_handler(lambda: print("device unplugged!"))  # fired off the audio thread
```

Full-duplex "mic → graph → speakers" on one clock:
```python
dup = a.DuplexBackend()
dup.open(ex, input_channels=1, output_channels=2, sample_rate=48000.0, block_size=256)
dup.start(); …; dup.stop()
```

C++ duplex:
```cpp
io::CoreAudioDuplexBackend dup;
io::StreamConfig c; c.inputChannels = 1; c.outputChannels = 2; c.sampleRate = 48000.0; c.blockSize = 256;
dup.open(c, &exec);
dup.start();   // … later … dup.stop();
```

**A device as the master clock for the multi-source manager.** Every device backend's `open()`
also accepts a `MasterClockAdapter` instead of an executor — then the device's IOProc pumps the
whole `MultiSourceManager` + multi-stream graph (§9), so other sources push into the manager's
rings off-thread while this device drives the clock. This is **live multi-source on real
hardware**, from Python:
```python
mgr = a.MultiSourceManager(2, 1, 1, 512, 48000)
ex.compile(graph_with_two_input_streams, channels=1, sample_rate=48000.0, max_block=512)
adapter = a.MasterClockAdapter(mgr, ex, in_stream=0, out_stream=0)
be.open(adapter, channels=1, sample_rate=48000.0, block_size=512)   # device → pumps the manager
be.start()
# … a producer thread keeps mgr.push_input(0/1, …) fed …
```

System / per-app capture:
```python
tap = a.TapBackend()
for p in a.TapBackend.list_processes():      # ProcessInfo: pid, bundle_id (permission-free)
    print(p.pid, p.bundle_id)
tap.tap_system_audio()                       # or tap.tap_process(pid)
tap.open(ex, channels=2, sample_rate=48000.0, block_size=512); tap.start()
```

### Mock backend (deterministic, headless)

Drives the executor by hand — the whole live path with no hardware (used in tests/CI):
```python
mock = a.MockBackend()                        # (drives a MasterClockAdapter — see §9)
```
In C++ a `MockBackend` can drive any `RenderCallback` (incl. the executor) via `tick(frames)`.

---

## 9. Multi-source & multi-clock

### MultiSourceManager — N inputs + M outputs on one clock

Each `(stream, channel)` crosses the thread boundary on its own lock-free ring. Producers
`push_input`, consumers `pop_output`, and a master clock (or manual pump) calls `process` to
drain inputs → run the multi-stream graph → fill outputs.

```python
# graph: source(0)+source(1) → sum → sink(0)
mgr = a.MultiSourceManager(num_inputs=2, num_outputs=1, channels=1, max_block=64, ring_frames=512)
mgr.push_input(0, np.full((1, 64), 0.5, np.float32))
mgr.push_input(1, np.full((1, 64), 0.25, np.float32))
mgr.process(ex, 64)                           # pump (GIL released)
print(mgr.pop_output(0, 64)[0, 0])            # ≈ 0.75
print(mgr.input_underruns(0), mgr.output_overruns(0))   # xrun telemetry
```

### MasterClockAdapter — drive the manager from any backend's clock

```python
adapter = a.MasterClockAdapter(mgr, ex, in_stream=0, out_stream=0)
dev = a.MockBackend()                         # or a real DeviceBackend in C++
dev.open(adapter, in_channels=1, out_channels=1, block_size=64)
dev.set_input_value(0.5); dev.start(); dev.tick(64)
print(dev.captured_output(0))                 # device → manager → graph → device
```

### CrossClockBridge — one input device + one output device on **separate** clocks

The master device defines the engine clock; the off-clock output device pulls through a
drift-compensated resampler that keeps the cross-clock ring bounded (ADR-0015).

```python
bridge = a.CrossClockBridge(mgr, ex, in_stream=0, out_stream=0,
                            engine_rate=48000.0, out_device_rate=48000.0,
                            channels=1, ring_frames=8192, max_block=64)
dev_in, dev_out = a.MockBackend(), a.MockBackend()
bridge.attach_master(dev_in, in_channels=1, out_channels=0, block_size=64)   # master clock
bridge.attach_output(dev_out, in_channels=0, out_channels=1, block_size=64)  # off-clock output
# tick dev_in (engine) and dev_out (its own clock) from their IOProcs …
print(bridge.output_ratio, bridge.output_fill, bridge.output_underruns)
```

### LiveMultiSource — **N** live sources on different clocks → one graph → a master output

The general case: several live inputs each on their **own** clock, mixed through one graph, out
to a master output device whose IOProc is the clock. `add_source` gives each source a ring +
drift servo; `attach_master_output` makes a device pump everything. Real backends attach too
(`attach_source(InputBackend|TapBackend, …)`, `attach_master_output(DeviceBackend, …)`) — this
is live multi-source on real hardware, from Python:

```python
ex.compile(graph_with_N_input_streams, channels=1, sample_rate=48000.0, max_block=256)
lms = a.LiveMultiSource(ex, engine_rate=48000.0, channels=1, max_block=256)
lms.attach_source(mic,   stream=0, source_rate=48000.0)      # each source on its own clock
lms.attach_source(other, stream=1, source_rate=44100.0)      # drift-compensated onto the engine
lms.attach_master_output(speakers, out_stream=0, channels=2, sample_rate=48000.0)
mic.start(); other.start(); speakers.start()                 # speakers' IOProc pumps the mix
print(lms.source_ratio(1), lms.source_fill(1), lms.source_underruns(1))   # per-source telemetry
```
Mock-verified headlessly and validated with one real source live (mic→speakers); the N-real-
device long drift soak is the remaining hardware step (`docs/76`).

**Recording the mix to a WAV.** `attach_wav_recorder(path)` taps the mixed master output to a
file **off the audio thread** (ADR-0004): the pump pushes each block into a lock-free ring, a
writer thread drains it to disk. Records until `stop_recording()`; a full ring (disk stalling)
drops + counts frames rather than blocking the pump. This is the *N live sources → gain → mix →
`out.wav` for a duration* recorder:

```python
lms.attach_wav_recorder("out.wav", format=a.WavFormat.Float32)   # arm before/after start
mic.start(); other.start(); speakers.start()
time.sleep(duration_seconds)                                     # <-- recording duration
lms.stop_recording()                                             # final drain + finalize
print(lms.recorded_frames, lms.record_dropped_frames)            # telemetry (dropped ≈ 0)
```

---

## 10. Boundary DSP utilities

These convert/align audio at the I/O edge (not graph nodes — the graph is single-rate).

```python
# Sample-rate conversion (stateful; feed consecutive blocks)
rs = a.Resampler(channels=1, ratio=44100/48000)      # ratio = in_rate/out_rate (<1 upsamples)
out, consumed = rs.process(block_44k, out_cap=1024)
print(rs.latency_frames)                             # kernel group delay

# Channel mapping at the device↔graph boundary (mono↔stereo / N↔M)
stereo = a.map_channels(mono_block, 2)               # Auto: 1→N duplicate, N→1 average
mono   = a.map_channels(stereo_block, 1, a.ChannelMapMode.DownmixToMono)

# Off-clock source: ring + resampler + drift servo (producer pushes, engine pulls)
src = a.ResamplingSource(channels=1, nominal_ratio=1.0, ring_frames=4096, max_block=64)
src.push(producer_block); engine_block = src.pull(64)
print(src.ratio, src.fill_frames, src.overruns, src.underruns)
```

WAV read/write is also bound directly (`WavReader`/`WavWriter`, see §8) for render loops and
recorders. C++-only building blocks (RT internals, deliberately not bound — ADR-0004):
`io::RingBuffer<T>` (lock-free SPSC), `io::int16ToFloat`/`floatToInt16`, `io::Resampler`,
`io::DriftCompensator`, `io::mapChannels`.

---

## 11. End-to-end recipes

**A synth voice (generator → filter → drive → gain)** — Python:
```python
g = a.Graph()
osc = g.add_oscillator("saw", 110.0, 0.6)
lp  = g.add_biquad_lowpass(1200.0, 0.9, 48000.0)
sat = g.add_waveshaper("tanh", 1.5, 1.0)
out = g.add_sink()
g.connect(osc, 0, lp, 0); g.connect(lp, 0, sat, 0); g.connect(sat, 0, out, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=48000.0, max_block=512)
audio = ex.process_multi([np.zeros((1, 512), np.float32)])[0]   # oscillator generates
```

**A vocal channel strip (HP → 3-band EQ → compressor → makeup)** — Python:
```python
g = a.Graph(); SR = 48000.0
s  = g.add_source()
hp = g.add_biquad_highpass(80.0, 0.707, SR)              # remove rumble
eq = g.add_parametric_eq([("lowshelf", 200, 0.7, -2.0),
                          ("peaking", 3000, 1.2, 3.0),    # presence
                          ("highshelf", 9000, 0.7, 2.0)], SR)
cmp = g.add_compressor(threshold_db=-18, ratio=3, attack_ms=8, release_ms=120)
k  = g.add_sink()
for x, y in [(s, hp), (hp, eq), (eq, cmp), (cmp, k)]: g.connect(x, 0, y, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)
ex.set_param(cmp, 4, 6.0)                                 # +6 dB makeup, live
```

**Offline master (file → EQ → limiter → file)** — Python:
```python
g = a.Graph(); SR = 48000.0
s   = g.add_source()
eq  = g.add_biquad_highshelf(8000.0, 0.7, 1.5, SR)
lim = g.add_compressor(threshold_db=-1.0, ratio=20.0, attack_ms=1, release_ms=50,
                       lookahead_frames=64)               # brickwall-ish limiter (reports latency)
k   = g.add_sink()
for x, y in [(s, eq), (eq, lim), (lim, k)]: g.connect(x, 0, y, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=2, sample_rate=SR, max_block=1024)
ob = a.OfflineBackend("mix.wav", "master.wav", a.WavFormat.Int16); ob.open(ex, 512); ob.start()
```

**Live monitor with FX (mic → gate → EQ → comp → speakers)** — C++ (macOS):
```cpp
Graph g;
const NodeId s  = g.addNode(std::make_unique<SourceNode>());
const NodeId gt = g.addNode(std::make_unique<GateNode>(-45.0f));
auto eq = std::make_unique<BiquadNode>(); eq->setPeaking(2500.0, 1.0, 4.0, 48000.0);
const NodeId eqN = g.addNode(std::move(eq));
const NodeId cp = g.addNode(std::make_unique<CompressorNode>(-20.0f, 4.0f, 5.0f, 80.0f));
const NodeId k  = g.addNode(std::make_unique<SinkNode>());
g.connect(s,0,gt,0); g.connect(gt,0,eqN,0); g.connect(eqN,0,cp,0); g.connect(cp,0,k,0);

GraphExecutor exec;
exec.compile(g, /*channels*/ 1, 48000.0, 256);
io::CoreAudioDuplexBackend dup;
io::StreamConfig c; c.inputChannels = 1; c.outputChannels = 1; c.sampleRate = 48000.0; c.blockSize = 256;
dup.open(c, &exec);
dup.start();   // mic → gate → EQ → compressor → speakers, live
```

More runnable examples: [`examples/cpp/`](../examples/cpp) and [`examples/python/`](../examples/python);
a guided every-feature pass: [`testing/notebooks/aiudio_acceptance_walkthrough.ipynb`](../testing/notebooks/aiudio_acceptance_walkthrough.ipynb).

---

## 12. What Phase 0 does *not* do yet

Honest boundaries (the audio thread stays C++/allocation-free, and Phase 0 built the
**spine + control frontend**, not the ML/agent layers):

- **No differentiable / trainable execution and no neural nodes** — Phase 1 / Tier 3 (`docs/78`).
- **No agent (NL → graph)** — Phase 2.
- **No reverb / spectral (FFT) / convolution / loudness meters** — Tier 2 (`docs/78`, needs an FFT primitive).
- **Graphs are DAG-only and single-rate**; feedback effects use internal state; cross-rate work
  happens at the I/O boundary (§10).
- **Live device backends are macOS-only** (Core Audio); capture/taps need TCC (+ signing for taps).
- **File I/O is WAV only**; live param control is **index-based** `set_param` (named setters TBD);
  **no graph serialization** (save/load) yet.
- **RT internals are not exposed to Python** (`RingBuffer`, `AudioBuffer`, `RenderCallback`) — by
  design (ADR-0004).
- The cross-clock multi-device path's logic is proven headlessly via the mock; **two real devices
  on separate physical clocks** is the hardware-verified remainder (`docs/76`).

For the planned path forward: README **Roadmap**, `docs/76` (multi-source), `docs/78` (node tiers).

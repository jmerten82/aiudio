# 81 — Pipeline Usage Patterns (cookbook)

> **Last updated:** 2026-06-30 · **Scope:** the canonical end-to-end ways to wire the aiudio
> pipeline, along each design axis (**offline ↔ live**, **one ↔ many sources**, **single ↔
> multiple clocks**, **live ↔ recorded output**), with **complete C++ and Python** for each.
> Grounded in merged code (**✓ Verified** unless noted). This is the "how to assemble a
> topology" doc; for the per-function API see [`docs/pipeline/80`](../pipeline/80-pipeline-capabilities.md), for the
> *why* see the ADRs, for the io primitives see [`docs/pipeline/72`](../pipeline/72-m1-aiudio-io-reference.md).

---

## Contents
- [0. The design axes](#0-the-design-axes)
- [How to read a pattern — the 6-question checklist](#how-to-read-a-pattern)
- [Pattern 1 — Offline render (file → graph → file)](#pattern-1--offline-render)
- [Pattern 2 — Offline multi-source mix (N files → graph → file)](#pattern-2--offline-multi-source-mix)
- [Pattern 3 — Live monitor, one source, shared clock (duplex)](#pattern-3--live-monitor-one-source-shared-clock)
- [Pattern 4 — Live multi-source on ONE clock → live output](#pattern-4--live-multi-source-on-one-clock)
- [Pattern 5 — Live multi-source on SEPARATE clocks → live output](#pattern-5--live-multi-source-on-separate-clocks)
- [Pattern 6 — Live multi-source, separate clocks, recorded (+ monitor)](#pattern-6--live-multi-source-recorded)
- [Pattern 7 — Live capture recorder, no playback (input-clock engine)](#pattern-7--live-capture-recorder-no-playback)
- [Appendix A — Who is the clock (the master)?](#appendix-a--who-is-the-clock)
- [Appendix B — Which sources need a ring?](#appendix-b--which-sources-need-a-ring)
- [Appendix C — RT-safety checklist for a custom orchestration callback](#appendix-c--rt-safety-checklist)
- [Appendix D — Cross-references](#appendix-d--cross-references)

---

## 0. The design axes

Every real use case is a point in this space. The table maps the patterns below onto the axes;
the **"engine"** column names the piece that owns the per-block orchestration.

| # | Pattern | Latency | Sources | Clocks | Output | Engine / driver |
|---|---|---|---|---|---|---|
| 1 | Offline render | offline | 1 | — (synchronous) | file | `OfflineBackend` |
| 2 | Offline multi-source mix | offline | N | — (synchronous) | file | your loop + `process_multi` |
| 3 | Live monitor, shared clock | live | 1 (+ its output) | 1 | device | `DuplexBackend` → `GraphExecutor` |
| 4 | Live multi-source, one clock | live | N (pushed) | 1 | device | `MasterClockAdapter` → `MultiSourceManager` |
| 5 | Live multi-source, separate clocks | live | N (devices) | N | device | `LiveMultiSource` |
| 6 | …recorded (+ monitor) | live | N (devices) | N | device **+ file** | `LiveMultiSource` + `attach_wav_recorder` |
| 7 | Live capture recorder, no playback | live | N (devices) | N | file only | a custom input-clock callback |

Two facts collapse most of the space:

- **Offline is inherently single-clock and its "output" is a file** — there is no hardware clock;
  a loop pumps `process()` as fast as it can. So the offline axis only varies by source count.
- **Live always has exactly one *master clock*** (ADR-0005): the one backend whose IOProc calls
  `process()`. Everything else that runs at a *different* clock must cross to the master through a
  **ring** (ADR-0008). The live axes vary by *how many other clocks* there are and *where the
  mixed output goes*.

---

## How to read a pattern

For any topology you build, answer these six questions — every pattern below answers them, and
your own variants should too. This *is* "what you need to know to implement a pattern."

1. **Who is the clock (the master)?** Exactly one backend's IOProc drives `process()` (ADR-0005).
   Offline: the synchronous pump. Live: pick the device whose timing everything aligns to
   (usually the output you play to, or — for a pure recorder — an input). See [Appendix A](#appendix-a--who-is-the-clock).
2. **How many sources, and does each cross a clock boundary?** A source on the *master's own*
   clock is delivered inline (no ring). A source on *any other* clock must go through a
   `ResamplingSource` (ring + resampler + drift servo). See [Appendix B](#appendix-b--which-sources-need-a-ring).
3. **Where does the output go?** A device (live/monitor), a file (recorded), or both. Recording is
   an **off-thread tap** (`WavRecorder`): the audio thread pushes into a ring, a writer thread
   drains to disk (ADR-0004).
4. **What runs on the audio thread, and what's pre-allocated?** `process()`, ring `push`/`pull`,
   and any node work are RT-safe and allocation-free. *All* buffers are allocated at
   `compile()`/`prepare()`/`open()`/`start()` — never in the callback. Nothing on the audio thread
   allocates, locks, does I/O, or calls Python. See [Appendix C](#appendix-c--rt-safety-checklist).
5. **What is the setup ordering?** Build+validate the `Graph` → `compile()` the executor → build
   the transport (manager/`LiveMultiSource`) → attach sources + output → (arm recorder) → `start()`.
   `LiveMultiSource` **must be constructed after** the executor is compiled (it reads the output-
   stream count).
6. **Platform / permissions?** Live device I/O is **macOS-only** (Core Audio). The mic needs the
   microphone TCC grant; the process **tap** needs `NSAudioCaptureUsageDescription` **and** a code
   signature (see [`docs/pipeline/70`](../pipeline/70-macos-audio-capture-plan.md) §6). Offline + numpy paths are
   cross-platform.

Conventions in the code below: audio is **planar float32**, shape `(channels, frames)`; `SR` is
the sample rate; `BLOCK` the block size. Python uses `import aiudio as a`; C++ uses
`namespace aiudio`.

---

## Pattern 1 — Offline render

**File → graph → file, faster than real time.** One input WAV, one processing chain, one output
WAV. The bread-and-butter batch case (master a stem, apply an EQ+compressor to a mixdown).

- **Clock:** the `OfflineBackend`'s synchronous pump (no hardware). **Sources:** 1 (the input
  file). **Rings:** none. **Output:** a file. **RT:** irrelevant — it's not real time, but the
  *same* `GraphExecutor` runs unchanged (ADR-0009).
- **Must know:** compile the executor with the file's **own channel count and sample rate**
  (`OfflineBackend` exposes them after construction); the render is bit-exact and deterministic;
  `start()` blocks until the whole file is rendered.

**Python**
```python
import aiudio as a

ob = a.OfflineBackend("in.wav", "out.wav", a.WavFormat.Float32)   # reads in.wav's header
assert ob.input_ok

g = a.Graph()
src = g.add_source()                      # stream 0 in, stream 0 out by default
eq  = g.add_biquad_lowpass(4000.0, 0.707, ob.input_sample_rate)
cmp = g.add_compressor(-18.0, 3.0, 5.0, 80.0)
snk = g.add_sink()
g.connect(src, 0, eq, 0); g.connect(eq, 0, cmp, 0); g.connect(cmp, 0, snk, 0)
assert g.validate()[0]

ex = a.GraphExecutor()
ex.compile(g, channels=ob.input_channels, sample_rate=ob.input_sample_rate, max_block=1024)
ob.open(ex, block_size=512)
ob.start()                                # renders to completion, synchronously
print("rendered", ob.frames_rendered, "frames")
```

**C++**
```cpp
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/graph/biquad_node.hpp"
#include "aiudio/graph/compressor_node.hpp"
#include "aiudio/io/offline_backend.hpp"
using namespace aiudio;

io::OfflineBackend ob{"in.wav", "out.wav", io::WavFormat::Float32};
if (!ob.inputOk()) return 1;

graph::Graph g;
const auto src = g.addNode(std::make_unique<graph::SourceNode>(0));
auto lp = std::make_unique<graph::BiquadNode>(ob.inputChannels());  // construct with channel count…
lp->setLowpass(4000.0, 0.707, ob.inputSampleRate());                // …then design the shape
const auto eq  = g.addNode(std::move(lp));
const auto cmp = g.addNode(std::make_unique<graph::CompressorNode>(-18.0f, 3.0f, 5.0f, 80.0f));
const auto snk = g.addNode(std::make_unique<graph::SinkNode>(0));
g.connect(src, 0, eq, 0); g.connect(eq, 0, cmp, 0); g.connect(cmp, 0, snk, 0);

graph::GraphExecutor exec;
exec.compile(g, ob.inputChannels(), ob.inputSampleRate(), /*maxBlock*/ 1024);
io::StreamConfig cfg; cfg.blockSize = 512;
ob.open(cfg, &exec);
ob.start();   // synchronous
```

---

## Pattern 2 — Offline multi-source mix

**N input files → one graph → one output file.** There is no multi-*input* offline backend
(`OfflineBackend` reads a single file), so you drive it yourself: read a block from each file,
feed the **multi-stream** executor, write the mix. This is the offline analogue of Pattern 5.

- **Clock:** your loop. **Sources:** N files. **Rings:** none (you already hold whole files —
  no clock to cross). **Output:** a file.
- **Must know:** the multi-stream call (`process_multi` / `process(ins,N,outs,M,…)`) takes one
  block *per input stream*, all the **same frame count**; readers hit EOF at different times, so
  **pad short blocks to the block size** and stop when *every* reader is empty. Bind
  `SourceNode(k)` to input stream `k`. Files must share a sample rate (resample at the boundary
  first if not — `Resampler`, `docs/pipeline/80` §10).

**Python**
```python
import numpy as np, aiudio as a

paths = ["mic.wav", "guitar.wav", "synth.wav"]
readers = [a.WavReader(p) for p in paths]
assert all(r.ok for r in readers)
SR, BLOCK, N = readers[0].sample_rate, 1024, len(readers)

g = a.Graph()
srcs  = [g.add_source(k) for k in range(N)]            # source k ← input stream k
gains = [g.add_gain(1.0 / N) for _ in range(N)]
mix, snk = g.add_sum(N), g.add_sink(0)
for k in range(N):
    g.connect(srcs[k], 0, gains[k], 0)
    g.connect(gains[k], 0, mix, k)
g.connect(mix, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

with a.WavWriter("mix.wav", channels=1, sample_rate=SR, format=a.WavFormat.Float32) as w:
    while True:
        blocks = [r.read(BLOCK) for r in readers]      # each (1, produced), 0 at EOF
        n = max(b.shape[1] for b in blocks)
        if n == 0:
            break
        ins = [np.pad(b, ((0, 0), (0, BLOCK - b.shape[1]))) for b in blocks]  # pad to BLOCK
        out = ex.process_multi(ins, num_outputs=1)[0]  # multi-stream render
        w.write(np.ascontiguousarray(out[:, :n]))      # write only the valid frames
```

**C++** (same shape; read into per-source scratch, pad, `process` multi-stream, write)
```cpp
#include <vector>
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/sum_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/io/wav_file.hpp"
using namespace aiudio;

const std::vector<std::string> paths = {"mic.wav", "guitar.wav", "synth.wav"};
const std::uint32_t N = paths.size(), BLOCK = 1024;
std::vector<std::unique_ptr<io::WavReader>> readers;
for (auto& p : paths) readers.push_back(std::make_unique<io::WavReader>(p));
const double SR = readers[0]->sampleRate();

graph::Graph g;
std::vector<graph::NodeId> srcs, gains;
const auto mix = g.addNode(std::make_unique<graph::SumNode>(N));
const auto snk = g.addNode(std::make_unique<graph::SinkNode>(0));
for (std::uint32_t k = 0; k < N; ++k) {
    srcs.push_back(g.addNode(std::make_unique<graph::SourceNode>(k)));
    gains.push_back(g.addNode(std::make_unique<graph::GainNode>(1.0f / N)));
    g.connect(srcs[k], 0, gains[k], 0);
    g.connect(gains[k], 0, mix, k);
}
g.connect(mix, 0, snk, 0);
graph::GraphExecutor exec;
exec.compile(g, /*channels*/ 1, SR, BLOCK);

// Per-source input buffers + one output buffer, all pre-sized to BLOCK (mono).
std::vector<std::vector<float>> inStore(N, std::vector<float>(BLOCK, 0.0f));
std::vector<float*> inPtr(N);
std::vector<io::AudioBuffer> ins(N);
for (std::uint32_t k = 0; k < N; ++k) { inPtr[k] = inStore[k].data();
    ins[k] = io::AudioBuffer{&inPtr[k], 1, BLOCK}; }
std::vector<float> outStore(BLOCK, 0.0f);
float* outPtr = outStore.data();
io::AudioBuffer out{&outPtr, 1, BLOCK};

io::WavWriter w{"mix.wav", 1, SR, io::WavFormat::Float32};
for (;;) {
    std::uint32_t n = 0;
    for (std::uint32_t k = 0; k < N; ++k) {
        std::fill(inStore[k].begin(), inStore[k].end(), 0.0f);        // pad
        float* ch = inStore[k].data();
        n = std::max(n, readers[k]->read(&ch, 1, BLOCK));             // fills [0, produced)
    }
    if (n == 0) break;
    exec.process(ins.data(), N, &out, 1, BLOCK, io::TimeInfo{});
    w.write(&outPtr, 1, n);                                           // valid frames only
}
w.finalize();
```

---

## Pattern 3 — Live monitor, one source, shared clock

**Mic → graph → speakers, in real time, on one shared clock** (`DuplexBackend`). The classic
input-monitoring / live-FX case: one device provides *both* the input and the output on a single
IOProc, so there is no clock mismatch and no ring.

- **Clock:** the duplex device's IOProc (one clock for in *and* out — an aggregate device if the
  chosen input and output differ). **Sources:** 1 (the mic, delivered as `in`). **Rings:** none.
  **Output:** the device. **RT:** the graph runs on the audio thread; drive parameter changes via
  `set_*` (a lock-free queue), never by mutating the graph from Python.
- **Must know:** the device↔graph **channel widths are mapped at the boundary** (M9.2) — a mono
  graph fans out to a stereo output (as `examples/cpp/ex_duplex_passthrough.cpp` shows); to
  process in true stereo, compile `channels=2` and widen mono sources with a `Pan`/`Upmix` node
  (G8). `latency_frames` reports round-trip latency for delay compensation.

**Python**
```python
import time, aiudio as a
SR, BLOCK = 48000.0, 128

g = a.Graph()
src = g.add_source()
gn  = g.add_gain(0.8)
lp  = g.add_biquad_lowpass(3000.0, 0.707, SR)
snk = g.add_sink()
g.connect(src, 0, gn, 0); g.connect(gn, 0, lp, 0); g.connect(lp, 0, snk, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

dux = a.DuplexBackend()
dux.open(ex, input_channels=1, output_channels=2, sample_rate=SR, block_size=BLOCK)  # mono in → stereo out
dux.start()
try:
    time.sleep(5.0)
    ex.set_cutoff(lp, 800.0)          # live, click-free param change (control-rate queue)
finally:
    dux.stop()
```

**C++**
```cpp
#include <chrono>
#include <thread>
#include "aiudio/graph/graph.hpp"
#include "aiudio/graph/graph_executor.hpp"
#include "aiudio/graph/source_node.hpp"
#include "aiudio/graph/gain_node.hpp"
#include "aiudio/graph/biquad_node.hpp"
#include "aiudio/graph/sink_node.hpp"
#include "aiudio/io/coreaudio_duplex_backend.hpp"
using namespace aiudio;
constexpr double SR = 48000.0; constexpr std::uint32_t BLOCK = 128;

graph::Graph g;
const auto src = g.addNode(std::make_unique<graph::SourceNode>(0));
const auto gn  = g.addNode(std::make_unique<graph::GainNode>(0.8f));
auto biquad = std::make_unique<graph::BiquadNode>(/*maxChannels*/ 1);
biquad->setLowpass(3000.0, 0.707, SR);
const auto lp  = g.addNode(std::move(biquad));
const auto snk = g.addNode(std::make_unique<graph::SinkNode>(0));
g.connect(src, 0, gn, 0); g.connect(gn, 0, lp, 0); g.connect(lp, 0, snk, 0);
graph::GraphExecutor exec; exec.compile(g, /*channels*/ 1, SR, BLOCK);

io::CoreAudioDuplexBackend dux;
io::StreamConfig cfg; cfg.inputChannels = 1; cfg.outputChannels = 2;
cfg.sampleRate = SR; cfg.blockSize = BLOCK;
dux.open(cfg, &exec);
dux.start();
std::this_thread::sleep_for(std::chrono::seconds(5));
dux.stop();
```

---

## Pattern 4 — Live multi-source on ONE clock

**N sources that all live on the *same* clock → mix → live output**, driven by one device via a
`MasterClockAdapter` over a `MultiSourceManager`. Use this when the extra sources are **produced
on the control side** (generated blocks, streamed/decoded audio, another thread) rather than by
their own hardware clock — the manager's rings buffer them and one device clock pumps the mix.

- **Clock:** one device (its IOProc drives `manager.process()` via the adapter). **Sources:** N
  input streams of the manager; a producer calls `push_input(stream, block)` (from the control
  thread or another RT thread). **Rings:** one SPSC ring per input stream (single clock, so **no
  resampler/drift servo** — this is `MultiSourceManager`, not `LiveMultiSource`). **Output:** the
  device (via `pop_output`, done inside the adapter).
- **Must know:** the adapter binds the device's *own* input to `in_stream` and its output to
  `out_stream`; other streams you feed yourself with `push_input`. Pre-fill the rings before
  `start()` so the first blocks aren't underruns. Pushing from Python is **control-rate** (fine
  for buffered/generated content; the ring absorbs jitter) — it is *not* sample-accurate RT.
  Watch `input_underruns(stream)` / `output_overruns(stream)`.

**Python** (device output is the clock; two synth streams pushed from Python)
```python
import time, numpy as np, aiudio as a
SR, BLOCK = 48000.0, 256

# graph: 2 input streams → mixer → sink(0)
g = a.Graph()
i0, i1 = g.add_source(0), g.add_source(1)
mx, o0 = g.add_mixer(2, 0.7), g.add_sink(0)
g.connect(i0, 0, mx, 0); g.connect(i1, 0, mx, 1); g.connect(mx, 0, o0, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

mgr = a.MultiSourceManager(num_inputs=2, num_outputs=1, channels=1, max_block=BLOCK, ring_frames=8192)
adapter = a.MasterClockAdapter(mgr, ex, in_stream=0, out_stream=0)   # device out is stream 0

# Pre-fill ~0.5 s of both sources so playback starts glitch-free.
def block(n, f): return (0.3 * np.sin(2*np.pi*f*np.arange(n)/SR)).astype(np.float32)[None, :]
for i in range(int(0.5 * SR) // BLOCK):
    mgr.push_input(0, block(BLOCK, 220.0))
    mgr.push_input(1, block(BLOCK, 277.0))

dev = a.DeviceBackend()
dev.open(adapter, channels=1, sample_rate=SR, block_size=BLOCK)   # the adapter overload
dev.start()
try:
    n = 0
    while dev.running and n < int(3.0 * SR):     # keep feeding for ~3 s
        if mgr.push_input(0, block(BLOCK, 220.0)): mgr.push_input(1, block(BLOCK, 277.0)); n += BLOCK
        time.sleep(BLOCK / SR * 0.5)             # feed a little ahead of real time
finally:
    dev.stop()
```

**C++**
```cpp
#include "aiudio/graph/multi_source_manager.hpp"
#include "aiudio/graph/master_clock_adapter.hpp"
#include "aiudio/io/coreaudio_backend.hpp"
// … build graph g (2 sources → mixer → sink) and compile `exec` at channels=1, SR, BLOCK …

graph::MultiSourceManager mgr(/*numInputs*/ 2, /*numOutputs*/ 1, /*channels*/ 1,
                              /*maxBlock*/ BLOCK, /*ringFrames*/ 8192);
graph::MasterClockAdapter adapter(mgr, exec, /*inStream*/ 0, /*outStream*/ 0);

// Pre-fill both source rings (planar mono blocks) with pushInput(stream, buf, frames) …

io::CoreAudioBackend dev;                 // output device = the clock
io::StreamConfig cfg; cfg.outputChannels = 1; cfg.sampleRate = SR; cfg.blockSize = BLOCK;
dev.open(cfg, &adapter);                  // the device IOProc drives the adapter → manager → graph
dev.start();
// … keep calling mgr.pushInput(...) from a producer thread; dev.stop() when done …
```

---

## Pattern 5 — Live multi-source on SEPARATE clocks

**N live input devices, each on its OWN hardware clock → one graph → a master output device.**
The flagship live case (`LiveMultiSource`): a mic on interface A, a second mic/line on interface
B, a system tap — each ticking independently — brought onto one engine timeline and mixed live to
the speakers.

- **Clock:** the **master output device** (its IOProc pumps everything). **Sources:** N input
  backends, each attached with `attach_source`. **Rings:** **one per source** — each is a
  `ResamplingSource` (ring + resampler + **drift servo**) that pulls the source onto the engine
  clock; the servo keeps each ring bounded as clocks drift (ADR-0015). **Output:** the master
  device.
- **Must know:** **compile the executor first** (`LiveMultiSource` reads its output-stream count
  at construction); bind `SourceNode(k)` to stream `k` and register sources densely `0..N-1`; give
  each source its `source_rate` (used as the nominal resample ratio); watch per-source telemetry
  (`source_ratio/fill/underruns/overruns`). Each device's IOProc is a *separate producer thread*;
  the master IOProc is the single consumer — the rings make that safe (SPSC). Real N-device drift
  over long runs is the one hardware-tuning question ([`docs/pipeline/76`](../pipeline/76-multi-source-io-roadmap.md)).

**Python**
```python
import time, aiudio as a
SR, BLOCK = 48000.0, 256

# graph: 2 input streams → mixer → sink(0)
g = a.Graph()
i0, i1 = g.add_source(0), g.add_source(1)
mx, o0 = g.add_mixer(2, 1.0), g.add_sink(0)
g.connect(i0, 0, mx, 0); g.connect(i1, 0, mx, 1); g.connect(mx, 0, o0, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)  # compile FIRST

lms = a.LiveMultiSource(ex, engine_rate=SR, channels=1, max_block=BLOCK)
mic_a, mic_b = a.InputBackend(), a.InputBackend()   # two real input devices
spk = a.DeviceBackend()                             # the master clock
lms.attach_source(mic_a, stream=0, source_rate=SR, channels=1, input_device="")            # default input
lms.attach_source(mic_b, stream=1, source_rate=SR, channels=1, input_device="<UID of B>")  # a specific device
lms.attach_master_output(spk, out_stream=0, channels=1, sample_rate=SR)
mic_a.start(); mic_b.start(); spk.start()           # speakers' IOProc pumps the mix
try:
    time.sleep(10.0)
    print(lms.source_ratio(1), lms.source_fill(1), lms.source_underruns(1))  # per-source telemetry
finally:
    spk.stop(); mic_b.stop(); mic_a.stop()
```

**C++**
```cpp
#include "aiudio/graph/live_multi_source.hpp"
#include "aiudio/io/coreaudio_input_backend.hpp"
#include "aiudio/io/coreaudio_backend.hpp"
// … build graph g (2 sources → mixer → sink), then: …

graph::GraphExecutor exec; exec.compile(g, /*channels*/ 1, SR, BLOCK);   // compile FIRST
graph::LiveMultiSource lms(exec, SR, /*channels*/ 1, BLOCK);

io::CoreAudioInputBackend micA, micB;
io::CoreAudioBackend spk;                                   // master clock (output device)
io::StreamConfig ic; ic.inputChannels = 1; ic.sampleRate = SR; ic.blockSize = BLOCK;
io::StreamConfig oc; oc.outputChannels = 1; oc.sampleRate = SR; oc.blockSize = BLOCK;
micA.open(ic, &lms.addSource(0, SR, /*ringFrames*/ 8192));  // returns the RenderCallback to drive
micB.open(ic, &lms.addSource(1, SR, 8192));
spk.open(oc, &lms.masterOutput(0));
micA.start(); micB.start(); spk.start();
// … run …  spk.stop(); micB.stop(); micA.stop();
```

---

## Pattern 6 — Live multi-source, recorded

**Pattern 5 + record the mix to a WAV, while still monitoring.** Add one call: `attach_wav_recorder`.

- **Clock / sources / rings:** exactly Pattern 5. **Output:** the master device **and** a file.
  The recorder is an **independent off-thread tap** — the master pump pushes each mixed block into
  the recorder's ring; a **writer thread** drains it to disk (ADR-0004). A full ring (disk stall)
  **drops-and-counts** rather than blocking the pump.
- **Must know:** arm with `attach_wav_recorder(path)` (before or after `start()`); the recording
  length is however long you run before `stop_recording()` (or teardown) — that's your **duration
  parameter**. Check `record_dropped_frames` (≈ 0 on a healthy disk). ⚠️ **Feedback caveat:** if a
  source is a *whole-system tap* and the master output is a system device, the tap will capture the
  monitored mix → a loop; for that combination prefer a *per-app* tap, monitor on headphones the
  tap doesn't see, or use **Pattern 7** (no playback).

**Python** (add to Pattern 5)
```python
lms.attach_wav_recorder("out.wav", format=a.WavFormat.Float32)   # arm the off-thread tap
mic_a.start(); mic_b.start(); spk.start()
time.sleep(duration_seconds)                                     # <-- recording duration
lms.stop_recording()                                             # final drain + finalize
print(lms.recorded_frames, lms.record_dropped_frames)
```

**C++** (add to Pattern 5)
```cpp
lms.recordToWav("out.wav", io::WavFormat::Float32, /*ringFrames*/ 48000);
micA.start(); micB.start(); spk.start();
std::this_thread::sleep_for(std::chrono::duration<double>(durationSeconds));   // duration
lms.stopRecording();                                                          // drain + finalize
```

---

## Pattern 7 — Live capture recorder, no playback

**Record mic + system audio (each on its own clock) to a WAV, with NO output device.** The
`aiudio-recorder.app` pattern (`examples/cpp/ex_record_mic_tap.cpp`). Because nothing is played to
an output, a whole-system tap can't capture our own output — **no feedback**. This is the way to
capture the whole desktop mix + mic to a file.

The twist: here the **master clock is an *input* device** (the mic). `LiveMultiSource`'s model
assumes an *output* device is the master and ignores the incoming `in`, so it doesn't fit — you
**interpose your own `RenderCallback`** (the "engine") that:

1. is driven by the **mic input backend's IOProc** (the clock), receiving the mic as `in`;
2. **pulls** each off-clock source (the tap) from its `ResamplingSource` onto the mic timeline;
3. marshals the streams and calls the **multi-stream** `executor.process(ins, N, outs, M, …)`;
4. **pushes** the mixed output into a `WavRecorder`;
5. never writes an `out` (there is no output device).

The mic (the master clock) needs **no ring** — it *is* the clock; only the off-clock tap rings.
This is exactly the six-question checklist made concrete. See [Appendix C](#appendix-c--rt-safety-checklist)
for the RT rules the engine callback must honor.

- **Must know:** the interposed callback owns pre-allocated stream buffers (allocate in its
  constructor, reuse every block — it's the single producer of its own scratch). The tap runs on a
  *different* thread than the mic IOProc: `tap.push` on the tap thread, `tap.pull` on the mic
  thread — SPSC-safe. Permissions: mic TCC **and** a **signed binary** with
  `NSAudioCaptureUsageDescription` for the tap (`docs/pipeline/70` §6). This has **no Python equivalent**:
  the tap requires a signed native binary, which the interpreter isn't — so the engine lives in
  C++. (Python drives the *output-master* variants — Patterns 5/6.)

**C++** — the engine callback (the load-bearing part), then the wiring:
```cpp
// The ENGINE: driven by the mic IOProc (the clock). Mic `in` is stream 0; the drift-compensated
// tap is stream 1; run the graph; push the mix to the recorder. Nothing is played.
class MicTapEngine final : public io::RenderCallback {
public:
    MicTapEngine(graph::GraphExecutor& ex, io::ResamplingSource& tap, io::WavRecorder& rec,
                 std::uint32_t maxBlock)
        : ex_(ex), tap_(tap), rec_(rec) {
        tapStore_.assign(maxBlock, 0.0f); tapPtr_ = tapStore_.data();      // pre-allocate (setup)
        tapBuf_ = io::AudioBuffer{&tapPtr_, 1, maxBlock};
        outStore_.assign(maxBlock, 0.0f); outPtr_ = outStore_.data();
        outBuf_ = io::AudioBuffer{&outPtr_, 1, maxBlock};
    }
    void process(const io::AudioBuffer& in, io::AudioBuffer& /*out*/, std::uint32_t frames,
                 const io::TimeInfo& t) noexcept override {                 // RT — no alloc/lock/IO
        tap_.pull(tapBuf_, frames);                                         // tap → mic timeline
        io::AudioBuffer ins[2] = {in, tapBuf_};                            // stream 0 = mic, 1 = tap
        io::AudioBuffer outs[1] = {outBuf_};
        ex_.process(ins, 2, outs, 1, frames, t);                           // gains + sum
        rec_.pushBlock(outBuf_, frames);                                   // mix → recorder ring
    }
private:
    graph::GraphExecutor& ex_; io::ResamplingSource& tap_; io::WavRecorder& rec_;
    std::vector<float> tapStore_, outStore_; float* tapPtr_{}; float* outPtr_{};
    io::AudioBuffer tapBuf_, outBuf_;
};

// A second tiny callback pushes the tap (its own clock) into the ResamplingSource.
struct TapToSource final : io::RenderCallback {
    io::ResamplingSource& src;
    void process(const io::AudioBuffer& in, io::AudioBuffer&, std::uint32_t f,
                 const io::TimeInfo&) noexcept override { if (in.numChannels) src.push(in, f); }
};

// WIRING: graph (source0=mic, source1=tap → gains → sum → sink), compile, then:
io::ResamplingSource tapSource;
tapSource.prepare(/*channels*/ 1, /*nominalRatio*/ 1.0, /*ringFrames*/ 1 << 15, BLOCK);
io::WavRecorder recorder;
recorder.start("out.wav", 1, SR, io::WavFormat::Float32, /*ringFrames*/ 48000, BLOCK);

TapToSource tapProducer{tapSource};
MicTapEngine engine(exec, tapSource, recorder, BLOCK);

io::CoreAudioProcessTapBackend tapBackend; tapBackend.tapSystemAudio();
io::CoreAudioInputBackend micBackend;                     // the clock
io::StreamConfig tc; tc.sampleRate = SR; tc.blockSize = BLOCK;
io::StreamConfig mc; mc.sampleRate = SR; mc.blockSize = BLOCK; mc.inputChannels = 1;
tapBackend.open(tc, &tapProducer);
micBackend.open(mc, &engine);                             // mic IOProc drives the engine
tapBackend.start(); micBackend.start();
std::this_thread::sleep_for(std::chrono::duration<double>(durationSeconds));   // duration
micBackend.stop(); tapBackend.stop(); recorder.stop();    // recorder drains + finalizes
```

> The full, signed, argument-parsing version is `examples/cpp/ex_record_mic_tap.cpp` → packaged as
> `aiudio-recorder.app`. See `examples/cpp/README.md` and `docs/pipeline/70` §6.

---

## Appendix A — Who is the clock?

Exactly one thing calls `process()` (ADR-0005). Pick it by asking *what timing must the output be
sample-accurate to?*

| Situation | Master clock |
|---|---|
| Offline / batch | the synchronous pump (no hardware) |
| Playing to speakers (with or without inputs) | the **output** device (Patterns 3–6) |
| Input + output on one interface | the **duplex** device (one clock for both; Pattern 3) |
| Pure recorder, no playback | an **input** device (the mic; Pattern 7) |

Everything on a *different* clock than the master crosses to it through a ring (Appendix B). The
master's own audio (a duplex device's input, a `MasterClockAdapter`'s bound input) is delivered
inline and needs no ring.

## Appendix B — Which sources need a ring?

- **Same clock as the master → no ring.** The duplex input (Pattern 3), the mic that *is* the
  master (Pattern 7), or blocks you feed synchronously (Patterns 1–2).
- **Different clock than the master → one ring per source.** Every `LiveMultiSource` source
  (Patterns 5–6) and the off-clock tap in Pattern 7 use a **`ResamplingSource`** = SPSC ring +
  resampler + drift servo. The producer is that source's IOProc; the consumer is the master pump.
- **Same-clock but cross-thread (control-fed) → a plain ring, no resampler.** `MultiSourceManager`
  streams (Pattern 4): one SPSC ring per stream, no drift servo (all one clock).
- **Recording → a ring on the *output* side.** `WavRecorder`: the pump pushes, a writer thread
  pops (Patterns 6–7).

Ring depth is `ring_frames` (default 8192 frames ≈ 171 ms per source; 48000 ≈ 1 s for the
recorder), rounded up to a power of two internally; see [`docs/pipeline/80`](../pipeline/80-pipeline-capabilities.md)
for sizing.

## Appendix C — RT-safety checklist

Any callback the audio thread runs — a node's `process()`, a `MasterClockAdapter`, or a custom
engine like `MicTapEngine` — must obey ADR-0004:

- [ ] **No allocation** in the callback. Pre-size every buffer in the constructor /
      `prepare()` / `compile()` / `start()` and reuse it.
- [ ] **No locks, no syscalls, no file/network I/O, no logging, no exceptions, no Python.**
- [ ] **Cross threads only via a lock-free ring or an atomic** — never a mutex or a condition
      variable *signalled from* the audio thread.
- [ ] **Bounded work per block** — no unbounded loops; clamp `frames` to the compiled `maxBlock`.
- [ ] **One producer, one consumer per ring** (SPSC). Verify who writes and who reads each ring is
      exactly one thread each. (For `MicTapEngine`: the tap thread pushes the tap ring, the mic
      thread pulls it and pushes the recorder ring, the writer thread pops it.)
- [ ] Mark the callback `noexcept`; return silence on any unexpected state rather than throwing.

## Appendix D — Cross-references

- **Per-function API + more recipes:** [`docs/pipeline/80`](../pipeline/80-pipeline-capabilities.md).
- **io primitives** (`RingBuffer`, `AudioBuffer`, `RenderCallback`, `AudioBackend`, `WavRecorder`):
  [`docs/pipeline/72`](../pipeline/72-m1-aiudio-io-reference.md), [`docs/pipeline/71`](../pipeline/71-io-layer-milestones.md).
- **Multi-source / cross-clock design + status:** [`docs/pipeline/76`](../pipeline/76-multi-source-io-roadmap.md).
- **macOS capture, taps, permissions & signing:** [`docs/pipeline/70`](../pipeline/70-macos-audio-capture-plan.md) §6.
- **Why (decisions):** ADR-0004 (audio thread sacred), ADR-0005 (one callback, swappable clock),
  ADR-0008 (per-source rings, aggregate-then-resample), ADR-0009 (one IR, many backends),
  ADR-0014 (multi-source manager), ADR-0015 (boundary resampling + drift).
- **Runnable examples:** `examples/python/` (`ex_wav_io`, `ex_multisource`, `ex_live_multisource`,
  `ex_record_multisource`), `examples/cpp/` (`ex_render_file_offline`, `ex_duplex_passthrough`,
  `ex_record_mic_tap`).

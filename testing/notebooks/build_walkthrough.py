"""Generate testing/notebooks/aiudio_acceptance_walkthrough.ipynb.

This is the SOURCE for the acceptance walkthrough notebook (the .ipynb is the
committed, executed artifact the test suite runs). Regenerate after editing:

    python testing/notebooks/build_walkthrough.py
    jupyter nbconvert --to notebook --execute --inplace \
        --ExecutePreprocessor.kernel_name=python3 \
        testing/notebooks/aiudio_acceptance_walkthrough.ipynb

The walkthrough steps through EVERY piece of Python-exposed functionality and, after
each, calls out its **shortcomings**. Every cell is CI-safe: the live-device cell
guards itself and skips cleanly where there's no audio device.
"""
import pathlib

import nbformat as nbf

nb = nbf.v4.new_notebook()
cells = []


def md(s):
    cells.append(nbf.v4.new_markdown_cell(s))


def code(s):
    cells.append(nbf.v4.new_code_cell(s))


md(r"""# aiudio — Acceptance Walkthrough (every Python feature, and its shortcomings)

This notebook is part of the **test suite** (`testing/`). Unlike the teaching tour
(`notebooks/aiudio_pipeline_tour.ipynb`), it is a **systematic, step-by-step pass over
all functionality the Python layer exposes today** — every class, method, and property —
and it **explicitly flags the shortcomings** of each (a ⚠️ callout after each section,
plus a consolidated matrix at the end).

It executes end-to-end with **zero errors** on any machine (the live-device step guards
itself), so it doubles as a runnable acceptance check. See `testing/README.md`.

**Contents**
1. Environment & the exposed API surface
2. `Graph` — building & validating the IR (every node factory)
3. `GraphExecutor` — compile, run on numpy, telemetry
4. The DSP nodes — gain, mix, meter, biquad
5. The control plane — live, RT-safe parameter edits (G7)
6. `DeviceBackend` — the live RT device frontend (macOS)
7. `OfflineBackend` + `WavFormat` — file rendering
8. Cross-backend determinism (the *one-IR-many-backends* invariant)
9. **Consolidated shortcomings matrix**
""")

# ---------------------------------------------------------------- 1. environment
md(r"""## 1. Environment & the exposed API surface

First, what does the package actually expose? (This is the entire Python API today.)""")
code(r"""import platform
import numpy as np
import aiudio

SR = 48000.0
print("aiudio", aiudio.__version__, "| Python", platform.python_version(), "|", platform.system())
print("public API (aiudio.__all__):", aiudio.__all__)
print("DeviceBackend present:", hasattr(aiudio, "DeviceBackend"), "(macOS-only)")""")
md(r"""> ⚠️ **Shortcoming.** The whole surface is six names: `Graph`, `GraphExecutor`,
> `OfflineBackend`, `WavFormat` (+ macOS `DeviceBackend`, `AudioDeviceInfo`). There is no
> Python access to the RT internals (`RingBuffer`, `AudioBuffer`, `RenderCallback`), no
> serialization, no differentiable layer, and no agent. Those are deliberate (the audio
> thread is C++; ADR-0002/0004) or not built yet (Phases 1–2).""")

# ---------------------------------------------------------------- 2. Graph
md(r"""## 2. `Graph` — building & validating the IR

A graph is nodes + edges. Every node-factory method returns an integer **node id**.
Here is **every factory the Python layer exposes**:""")
code(r"""g = aiudio.Graph()
src   = g.add_source()                       # 0 in,  1 out
sink  = g.add_sink()                         # 1 in,  0 out
gain  = g.add_gain(0.5)                      # 1 in,  1 out
summ  = g.add_sum(2)                         # N in,  1 out (mixer)
meter = g.add_meter()                        # 1 in,  1 out (passthrough + level)
lp    = g.add_biquad_lowpass(1000.0, 0.707, SR)  # 1 in, 1 out (RBJ low-pass)
print("node factories ->", dict(source=src, sink=sink, gain=gain, sum=summ, meter=meter, biquad=lp))
print("node_count:", g.node_count)""")

md(r"""`connect(src, src_port, dst, dst_port)` wires output ports to input ports, and
`validate()` returns `(ok, error)`.""")
code(r"""g2 = aiudio.Graph()
s, gn, k = g2.add_source(), g2.add_gain(0.5), g2.add_sink()
print("connect ok:", g2.connect(s, 0, gn, 0), g2.connect(gn, 0, k, 0))
print("validate (good DAG):", g2.validate())""")

md(r"""`validate()` is also the gatekeeper. Two failure modes:""")
code(r"""# (a) a cycle
gc = aiudio.Graph(); a, b = gc.add_gain(1.0), gc.add_gain(1.0)
gc.connect(a, 0, b, 0); gc.connect(b, 0, a, 0)
print("cycle      ->", gc.validate())

# (b) two edges into one input port (you must mix with a SumNode instead)
gm = aiudio.Graph(); s2, x, y, k2 = gm.add_source(), gm.add_gain(1.0), gm.add_gain(1.0), gm.add_sink()
gm.connect(s2, 0, x, 0); gm.connect(s2, 0, y, 0)
gm.connect(x, 0, k2, 0); gm.connect(y, 0, k2, 0)   # both drive sink:0
print("double-drive->", gm.validate())""")
md(r"""> ⚠️ **Shortcoming.** Only **six** node factories exist. The C++ `BiquadNode` also does
> *high-pass*, but there's no `add_biquad_highpass`; there's no generic `add_node(...)`, and
> no EQ/reverb/dynamics/**neural** nodes yet. Growing the node library is Phase 1+. Also:
> graphs are **DAG-only** (no feedback cycles) and single-rate.""")

# ---------------------------------------------------------------- 3. executor
md(r"""## 3. `GraphExecutor` — compile, run on numpy, telemetry

`compile(graph, channels, sample_rate, max_block) -> bool` builds a static schedule;
`process((channels, frames) float32) -> (channels, frames) float32` runs one block.""")
code(r"""ex = aiudio.GraphExecutor()
print("compiled (before):", ex.compiled)
ok = ex.compile(g2, channels=1, sample_rate=SR, max_block=512)
print("compile ok:", ok, "| compiled:", ex.compiled)

y = ex.process(np.ones((1, 256), np.float32))
print("process: ones * gain 0.5 ->", float(y[0, 0]), "| out shape:", y.shape)
print("render_count (telemetry):", ex.render_count)""")
md(r"""> ⚠️ **Shortcoming — the `max_block` contract is a footgun.** `process()` silently
> renders only up to `max_block` frames; pass a bigger block and the tail comes back
> **zero** (no error). Demonstration:""")
code(r"""ex_small = aiudio.GraphExecutor()
ex_small.compile(g2, channels=1, sample_rate=SR, max_block=64)   # compiled for 64
out = ex_small.process(np.ones((1, 128), np.float32))            # but we pass 128
print("frames 0..63 (processed):", float(out[0, 0]), "| frames 64..127 (dropped→0):", float(out[0, 100]))
print("xrun_count:", ex_small.xrun_count, "— the under-served block is detected, silenced, and counted (M9.1)")""")

md(r"""**Multiple input/output streams (G10).** A single graph can receive **N input
streams** and drive **M output streams**: `add_source(stream=k)` / `add_sink(stream=k)` bind
a node to a stream, and `process_multi([...])` takes one array per input stream and returns
one per output stream. (`process(arr)` is the back-compatible 1-stream case.) This is the
graph-side foundation of true multi-source I/O ([`docs/76`](../../docs/76-multi-source-io-roadmap.md)).""")
code(r"""g_ms = aiudio.Graph()
a0, a1 = g_ms.add_source(stream=0), g_ms.add_source(stream=1)   # two input streams
g_a, g_b = g_ms.add_gain(0.5), g_ms.add_gain(0.25)             # asymmetric → proves routing
mix = g_ms.add_sum(2)
k0, k1 = g_ms.add_sink(stream=0), g_ms.add_sink(stream=1)       # two output streams
g_ms.connect(a0, 0, g_a, 0); g_ms.connect(a1, 0, g_b, 0)
g_ms.connect(g_a, 0, mix, 0); g_ms.connect(g_b, 0, mix, 1)
g_ms.connect(mix, 0, k0, 0); g_ms.connect(mix, 0, k1, 0)        # mix fans out to both outputs
ex_ms = aiudio.GraphExecutor(); ex_ms.compile(g_ms, channels=1, sample_rate=SR, max_block=128)
print("input_streams:", ex_ms.input_streams, " output_streams:", ex_ms.output_streams)
outs = ex_ms.process_multi([np.ones((1, 16), np.float32), 2*np.ones((1, 16), np.float32)])
print("mix 1·0.5 + 2·0.25 =", float(outs[0][0, 0]), "| #outputs:", len(outs))""")

md(r"""**Multi-source manager (M10).** The `MultiSourceManager` composes **N input sources +
M output sinks onto one clock**, each (stream, channel) crossing through its own lock-free
ring (ADR-0008 §5): producers `push_input`, the pump `process`-es the multi-stream graph,
consumers `pop_output`. Here Python plays all the roles (no device needed); per-stream xrun
telemetry comes for free.""")
code(r"""g_msm = aiudio.Graph()
a0, a1 = g_msm.add_source(0), g_msm.add_source(1)
mixn, kk = g_msm.add_sum(2), g_msm.add_sink(0)
g_msm.connect(a0, 0, mixn, 0); g_msm.connect(a1, 0, mixn, 1); g_msm.connect(mixn, 0, kk, 0)
ex_msm = aiudio.GraphExecutor(); ex_msm.compile(g_msm, channels=1, sample_rate=SR, max_block=64)

mgr = aiudio.MultiSourceManager(num_inputs=2, num_outputs=1, channels=1, max_block=64, ring_frames=256)
mgr.push_input(0, np.full((1, 32), 0.5, np.float32))   # source A
mgr.push_input(1, np.full((1, 32), 0.3, np.float32))   # source B
mgr.process(ex_msm, 32)                                 # the pump (one clock tick)
print("2 sources -> mix -> 1 sink:", float(mgr.pop_output(0, 32)[0, 0]), "(0.5 + 0.3 = 0.8)")
print("input underruns:", mgr.input_underruns(0), mgr.input_underruns(1))""")

# ---------------------------------------------------------------- 4. nodes
md(r"""## 4. The DSP nodes — gain, mix, meter, biquad

**Gain & mixing (fan-out + fan-in via `SumNode`):** source fans out to two gains that a
sum node mixes — `1·0.5 + 1·0.3 = 0.8`.""")
code(r"""gx = aiudio.Graph()
s = gx.add_source(); ga = gx.add_gain(0.5); gb = gx.add_gain(0.3); mix = gx.add_sum(2); k = gx.add_sink()
gx.connect(s, 0, ga, 0); gx.connect(s, 0, gb, 0)
gx.connect(ga, 0, mix, 0); gx.connect(gb, 0, mix, 1); gx.connect(mix, 0, k, 0)
exx = aiudio.GraphExecutor(); exx.compile(gx, channels=1, sample_rate=SR, max_block=32)
print("0.5 + 0.3 =", float(exx.process(np.ones((1, 16), np.float32))[0, 0]))""")

md(r"""**Meter** — `meter_mean_square(node)` reads the last block's level (an atomic the
audio thread publishes); convert to dBFS:""")
code(r"""gmt = aiudio.Graph(); s = gmt.add_source(); mt = gmt.add_meter(); k = gmt.add_sink()
gmt.connect(s, 0, mt, 0); gmt.connect(mt, 0, k, 0)
exm = aiudio.GraphExecutor(); exm.compile(gmt, channels=1, sample_rate=SR, max_block=1024)
for amp in (1.0, 0.5, 0.1):
    exm.process((amp * np.ones((1, 1024))).astype(np.float32))
    ms = gmt.meter_mean_square(mt)
    print(f"amp {amp:>4}  ->  meter_ms={ms:.4f}  ({10*np.log10(max(ms,1e-12)):6.1f} dBFS)")""")

md(r"""**Biquad low-pass** — a real stateful RBJ filter; a 5 kHz tone is attenuated, a 300 Hz
tone passes:""")
code(r"""gb2 = aiudio.Graph(); s = gb2.add_source(); lp2 = gb2.add_biquad_lowpass(1000.0, 0.707, SR); k = gb2.add_sink()
gb2.connect(s, 0, lp2, 0); gb2.connect(lp2, 0, k, 0)
exb = aiudio.GraphExecutor(); exb.compile(gb2, channels=1, sample_rate=SR, max_block=2048)
t = np.arange(2048) / SR
for f in (300.0, 5000.0):
    tone = np.sin(2 * np.pi * f * t).astype(np.float32).reshape(1, 2048)
    out = exb.process(tone)
    print(f"{f:>6.0f} Hz: in RMS {np.sqrt((tone**2).mean()):.3f} -> out RMS {np.sqrt((out**2).mean()):.3f}")""")
md(r"""> ⚠️ **Shortcoming.** This is most of the DSP palette today. No high-pass factory, no
> parametric EQ, dynamics, delay/reverb, or any neural node, and no general routing/mix-matrix
> node yet (beyond `SumNode` + the channel-width nodes below).""")

md(r"""**Channel-width nodes (G8) — per-port channel counts.** A node can *change* the channel
count: `add_downmix()` collapses N channels → 1 (mono average), `add_upmix(channels)` raises
1 → N (duplicate). The executor sizes each interior port to its own width; the numpy I/O
boundary stays at the host width. Here a stereo signal is down-mixed to mono then up-mixed
back to stereo:""")
code(r"""g_ch = aiudio.Graph()
s = g_ch.add_source(); dn = g_ch.add_downmix(); up = g_ch.add_upmix(2); k = g_ch.add_sink()
g_ch.connect(s, 0, dn, 0); g_ch.connect(dn, 0, up, 0); g_ch.connect(up, 0, k, 0)
ex_ch = aiudio.GraphExecutor(); ex_ch.compile(g_ch, channels=2, sample_rate=SR, max_block=64)
stereo = np.zeros((2, 8), np.float32); stereo[0] = 1.0; stereo[1] = 0.5   # L=1.0, R=0.5
y = ex_ch.process(stereo)
print("down->up: out[0]=", float(y[0, 0]), " out[1]=", float(y[1, 0]), " (mono (L+R)/2 = 0.75, duplicated)")""")

md(r"""**Latency & delay compensation (G9).** A node can declare a processing latency
(`add_latency(frames)` models a lookahead/FFT/resampler); `ex.latency_frames` reports the
graph total, and the executor **auto-aligns parallel branches** so they recombine in phase.
Here an impulse fans out through a `latency(8)` branch and a direct branch into a mixer —
the direct branch is compensated by 8, so a single **2×** impulse lands at frame 8 (not two
separate impulses at 0 and 8):""")
code(r"""g_lat = aiudio.Graph()
s = g_lat.add_source(); lat = g_lat.add_latency(8, 1); mix = g_lat.add_sum(2); k = g_lat.add_sink()
g_lat.connect(s, 0, lat, 0); g_lat.connect(lat, 0, mix, 0)   # branch A: +8 frames latency
g_lat.connect(s, 0, mix, 1)                                   # branch B: direct (compensated)
g_lat.connect(mix, 0, k, 0)
ex_lat = aiudio.GraphExecutor(); ex_lat.compile(g_lat, channels=1, sample_rate=SR, max_block=32)
imp = np.zeros((1, 32), np.float32); imp[0, 0] = 1.0
y = ex_lat.process(imp)[0]
print("latency_frames:", ex_lat.latency_frames, "| out[0]:", float(y[0]), " out[8]:", float(y[8]),
      " (realigned: single 2x impulse at 8)")""")

# ---------------------------------------------------------------- 5. control plane
md(r"""## 5. The control plane — live, RT-safe parameter edits (G7)

While the engine runs, parameters are changed by **enqueuing** onto a lock-free queue the
audio thread drains at the top of the next block. `set_gain` / `set_cutoff` / `set_q`, and
the generic `set_param(node, index, value)`, all return `False` only if the queue is full.""")
code(r"""gg = aiudio.Graph(); s = gg.add_source(); gn = gg.add_gain(0.5); k = gg.add_sink()
gg.connect(s, 0, gn, 0); gg.connect(gn, 0, k, 0)
exg = aiudio.GraphExecutor(); exg.compile(gg, channels=1, sample_rate=SR, max_block=64)
dc = np.ones((1, 64), np.float32)

print("gain 0.5                ->", float(exg.process(dc)[0, 0]))
print("set_gain(0.25):", exg.set_gain(gn, 0.25), "-> next block:", float(exg.process(dc)[0, 0]))
print("set_param(gain idx 0, 0.1):", exg.set_param(gn, 0, 0.1), "-> next block:", float(exg.process(dc)[0, 0]))
print("render_count (telemetry):", exg.render_count)""")

md(r"""`set_cutoff` / `set_q` drive a live `BiquadNode` the same way — here a 6 kHz tone is
blocked by a tight 500 Hz low-pass, then passes once we open the cutoff to 18 kHz, with no
recompile or stop:""")
code(r"""gq = aiudio.Graph(); s = gq.add_source(); lpc = gq.add_biquad_lowpass(500.0, 0.707, SR); k = gq.add_sink()
gq.connect(s, 0, lpc, 0); gq.connect(lpc, 0, k, 0)
exq = aiudio.GraphExecutor(); exq.compile(gq, channels=1, sample_rate=SR, max_block=256)
t = np.arange(256) / SR; tone = np.sin(2 * np.pi * 6000 * t).astype(np.float32).reshape(1, 256)
for _ in range(8): lo = exq.process(tone)         # settle at cutoff 500 Hz
print("set_cutoff(18000):", exq.set_cutoff(lpc, 18000.0), "| set_q(2.0):", exq.set_q(lpc, 2.0))
for _ in range(8): hi = exq.process(tone)         # settle at cutoff 18 kHz
print(f"6 kHz tone RMS: cutoff 500 Hz {np.sqrt((lo**2).mean()):.3f} -> 18 kHz {np.sqrt((hi**2).mean()):.3f}")""")

md(r"""> ⚠️ **Shortcoming — the queue is bounded and drops on overflow.** If you post far faster
> than the audio thread drains, `set_*` returns `False` and the change is dropped (never
> blocks — that's the RT-safety trade). Demonstration (flood without processing):""")
code(r"""exf = aiudio.GraphExecutor(); exf.compile(gg, channels=1, sample_rate=SR, max_block=64)
accepted = sum(1 for _ in range(8192) if exf.set_gain(gn, 0.5))  # flood without draining
print(f"accepted {accepted} of 8192 queued edits; the rest were dropped")
print("dropped_commands telemetry:", exf.dropped_commands, "(M9.1)")""")
md(r"""> ⚠️ **More shortcomings.** Only `gain` / `cutoff` / `Q` are controllable — there is no
> parameter **automation/modulation** (time-varying curves), just one-shot setters. And
> `Graph.set_gain(...)` (editing the IR node directly) still exists but is **only safe while
> stopped** — on a *running* device use the executor's queue (`ex.set_gain`), which is the
> RT-safe path.""")

# ---------------------------------------------------------------- 6. device backend
md(r"""## 6. `DeviceBackend` — the live RT device frontend (macOS)

`enumerate()` lists devices; `open/start/stop` run a real Core Audio output stream whose
audio thread is **pure C++** — Python only commands and observes. `AudioDeviceInfo` carries
the device fields.""")
code(r"""if hasattr(aiudio, "AudioDeviceInfo") and hasattr(aiudio, "DeviceBackend"):
    devices = aiudio.DeviceBackend().enumerate()
    print(f"{len(devices)} devices:")
    for d in devices:
        tag = "default-out" if d.is_default_output else "default-in" if d.is_default_input else ""
        print(f"  {d.name!r:<40} id={d.id[:8]}… in={d.input_channels} out={d.output_channels} "
              f"rates={len(d.sample_rates)} {tag}")
else:
    print("DeviceBackend not built on this platform (macOS-only).")""")

md(r"""**Input / duplex / tap backends (M11a).** Beyond the output `DeviceBackend`, the
**`InputBackend`** (mic capture), **`DuplexBackend`** (mic → graph → speakers on one clock),
and **`TapBackend`** (system / per-app *output* capture) are now bound — same control-frontend
pattern. Enumeration and process listing are permission-free; *capturing* needs mic TCC (and
taps need a signed binary). Run [`examples/python/ex_live_input.py`](../../examples/python/ex_live_input.py)
for a live mic meter.""")
code(r"""if hasattr(aiudio, "InputBackend"):
    inputs = [d for d in aiudio.InputBackend().enumerate() if d.input_channels > 0]
    print("input devices:", [d.name for d in inputs][:4])
    procs = aiudio.TapBackend.list_processes()          # permission-free
    print(f"tappable processes: {len(procs)} (e.g. {procs[0].bundle_id!r} pid={procs[0].pid})")
else:
    print("input/duplex/tap backends are macOS-only.")""")

md(r"""Starting a stream needs a real output device, so the next cell **guards itself** and
skips cleanly where there is none (e.g. CI). Where a device exists it runs ~0.4 s of
**silent** output and reports telemetry.""")
code(r"""import time
ran = False
if hasattr(aiudio, "DeviceBackend"):
    be = aiudio.DeviceBackend()
    outs = [d for d in be.enumerate() if d.output_channels > 0]
    if outs:
        gD = aiudio.Graph(); s = gD.add_source(); gnD = gD.add_gain(0.5); mD = gD.add_meter(); kD = gD.add_sink()
        gD.connect(s, 0, gnD, 0); gD.connect(gnD, 0, mD, 0); gD.connect(mD, 0, kD, 0)
        exD = aiudio.GraphExecutor(); exD.compile(gD, channels=2, sample_rate=SR, max_block=512)
        try:
            if be.open(exD, channels=2, sample_rate=SR, block_size=512):
                be.start()
                time.sleep(0.4)
                print(f"running={be.running}  latency_frames={be.latency_frames}  "
                      f"render_count={exD.render_count}  meter_ms={gD.meter_mean_square(mD):.2e}")
                exD.set_gain(gnD, 0.2); exD.set_cutoff(mD, 1000.0)  # live edits accepted off-thread
                be.stop()
                print("stopped; running =", be.running)
                ran = True
            else:
                print("open() returned False — skipping live start")
        except Exception as e:   # noqa: BLE001 - any device error -> skip, keep the notebook clean
            print("live start skipped:", e)
if not ran:
    print("(enumerate-only — no live device run on this machine)")""")
md(r"""> ⚠️ **Shortcomings (the big ones).**
> - **Live output is silent.** There is no oscillator / file-source node, and an output
>   device hands the graph an *empty* input — so the meter reads ~0. The demo proves the
>   *control + telemetry frontend*, not audible processing.
> - **Capturing needs permission.** `InputBackend`/`DuplexBackend` now bound (M11a), but a
>   live mic capture needs microphone TCC; `TapBackend` (system/app capture) needs a signed
>   binary + audio-capture TCC. Enumeration / process listing are permission-free.
> - **macOS only** (these Core Audio backends aren't present on other platforms).""")

# ---------------------------------------------------------------- 7. offline
md(r"""## 7. `OfflineBackend` + `WavFormat` — file rendering

Render a graph over a WAV file faster than real time. `WavFormat` selects the output
encoding. The same `GraphExecutor` that drives a device drives this — only the clock differs.""")
code(r"""import wave, tempfile, os
print("WavFormat options:", aiudio.WavFormat.Int16, aiudio.WavFormat.Float32)

tmp = tempfile.mkdtemp()
in_wav, out_wav = os.path.join(tmp, "in.wav"), os.path.join(tmp, "out.wav")
tt = np.arange(int(SR)) / SR
sig = (0.4 * np.sin(2 * np.pi * 300 * tt) + 0.4 * np.sin(2 * np.pi * 5000 * tt)).astype(np.float32)
with wave.open(in_wav, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(SR))
    w.writeframes((np.clip(sig, -1, 1) * 32767).astype("<i2").tobytes())

go = aiudio.Graph(); s = go.add_source(); lpo = go.add_biquad_lowpass(800.0, 0.707, SR); k = go.add_sink()
go.connect(s, 0, lpo, 0); go.connect(lpo, 0, k, 0)
ob = aiudio.OfflineBackend(in_wav, out_wav, aiudio.WavFormat.Int16)
print("input_ok:", ob.input_ok, "| channels:", ob.input_channels,
      "| sample_rate:", ob.input_sample_rate, "| frames:", ob.input_frames)
exo = aiudio.GraphExecutor(); exo.compile(go, channels=ob.input_channels, sample_rate=ob.input_sample_rate, max_block=1024)
ob.open(exo, block_size=512); ob.start()
print("frames_rendered:", ob.frames_rendered)""")
md(r"""> ⚠️ **Shortcoming — `WavFormat.Float32` output is unreadable by Python's stdlib `wave`.**
> Use `Int16` if you read it back with `wave` (the float path is valid WAV, just format-3,
> which `wave` rejects). Demonstration:""")
code(r"""f32 = os.path.join(tmp, "f32.wav")
gf = aiudio.Graph(); sf = gf.add_source(); kf = gf.add_sink(); gf.connect(sf, 0, kf, 0)
exff = aiudio.GraphExecutor(); exff.compile(gf, channels=1, sample_rate=SR, max_block=1024)
ob_f = aiudio.OfflineBackend(in_wav, f32, aiudio.WavFormat.Float32)
ob_f.open(exff, block_size=512); ob_f.start()
print("Float32 render frames_rendered:", ob_f.frames_rendered)
try:
    with wave.open(f32, "rb") as r:
        r.readframes(1)
    print("Float32 read OK (unexpected)")
except Exception as e:   # noqa: BLE001
    print("stdlib wave rejects Float32 WAV:", e, "-> use WavFormat.Int16 for round-trips")""")
md(r"""> ⚠️ **Shortcoming.** File I/O is **WAV only** (no FLAC/AIFF/MP3), and there's no streaming
> file reader from Python beyond the whole-file `OfflineBackend`.""")

# ---------------------------------------------------------------- 8. cross-backend
md(r"""## 8. Cross-backend determinism (the *one-IR-many-backends* invariant)

The headline architecture guarantee (ADR-0009): the **same** compiled graph produces the
**same** output on the numpy executor and the offline file backend.""")
code(r"""# numpy reference
gA = aiudio.Graph(); s = gA.add_source(); lpA = gA.add_biquad_lowpass(800.0, 0.707, SR); kA = gA.add_sink()
gA.connect(s, 0, lpA, 0); gA.connect(lpA, 0, kA, 0)
exA = aiudio.GraphExecutor(); exA.compile(gA, channels=1, sample_rate=SR, max_block=512)
ref = np.zeros(len(sig), np.float32)
for i in range(0, len(sig), 512):
    b = sig[i:i+512].reshape(1, -1); ref[i:i+b.shape[1]] = exA.process(b)[0]
# offline output from §7 (same graph), read back
with wave.open(out_wav, "rb") as r:
    off = np.frombuffer(r.readframes(r.getnframes()), "<i2").astype(np.float32) / 32768.0
n = min(len(ref), len(off))
print(f"max|numpy - offline| = {np.max(np.abs(ref[:n]-off[:n])):.2e}  (int16 quantum {1/32768:.2e}) -> identical")""")

# ---------------------------------------------------------------- 9. matrix
md(r"""## 9. Consolidated shortcomings matrix

Everything the Python layer **cannot** do yet (or does with a caveat), and why:

| Area | Shortcoming | Status / why |
|---|---|---|
| **Node library** | `source/sink/gain/sum/meter/biquad_lowpass/downmix/upmix/latency`; no high-pass factory, EQ, dynamics, delay/reverb, general routing-matrix, or neural nodes | grows in Phase 1+ |
| **Signal generation** | no oscillator/file-source node → **live device output is silent** | small follow-up |
| **Live input** | mic / full-duplex / process-tap backends **✅ bound** (`InputBackend`/`DuplexBackend`/`TapBackend`, M11a) — capture needs mic TCC; taps need a signed binary | ✅ |
| **Device backends** | output + input + duplex + tap bound (M11a), **macOS-only**; capture/tap need TCC (+ signed binary for taps) | scope / platform |
| **Control** | only gain/cutoff/Q; **no automation curves**; bounded queue **drops on overflow** (`set_* → False`) | future / RT trade-off |
| **Direct edits** | `Graph.set_gain` is **not RT-safe on a running stream** — use `ex.set_*` | by design |
| **`process()`** | silently renders only up to `max_block` frames (tail → 0) | contract footgun |
| **File I/O** | **WAV only**; `WavFormat.Float32` unreadable by stdlib `wave` (use `Int16`) | format scope |
| **Serialization** | no save/load of a graph | ADR-0009 deferred |
| **Differentiable / trainable** | not available | Phase 1 |
| **Agent (NL → graph)** | not available | Phase 2 |
| **Graph shape** | DAG-only (no feedback cycles), single sample-rate | ADR-0009 scope |
| **RT internals** | `RingBuffer` / `AudioBuffer` / `RenderCallback` not exposed | internal C++ (ADR-0004) |

**Why so much is "not yet":** the audio thread must stay C++ and allocation-free
(ADR-0004), and Phase 0 deliberately built the *spine + control frontend* first. Phases 1–2
add the differentiable core and the agent on top of exactly the hooks shown above (the
command queue + the atomic graph swap). See `testing/README.md` and `docs/74`.""")

nb["cells"] = cells
nb["metadata"]["kernelspec"] = {"display_name": "Python 3", "language": "python", "name": "python3"}
nb["metadata"]["language_info"] = {"name": "python"}

out = pathlib.Path(__file__).resolve().parent / "aiudio_acceptance_walkthrough.ipynb"
with open(out, "w") as f:
    nbf.write(nb, f)
print(f"wrote {out} with {len(cells)} cells")

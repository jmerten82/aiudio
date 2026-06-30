"""Generate notebooks/aiudio_feature_tutorial.ipynb — the full-feature teaching tutorial.

A friendly, **plotted**, end-to-end tour of aiudio that touches every relevant *mode of
operation* (numpy / offline-file / live-device / headless-mock / multi-source / cross-clock)
and *feature* (the whole node library, the control plane, graph editing, boundary DSP). Unlike
`testing/notebooks/aiudio_acceptance_walkthrough.ipynb` (a terse feature+shortcomings
checklist that is the CI acceptance test), this is the **teaching** notebook: prose, intuition,
and matplotlib figures.

It is **CI-safe**: every cell runs headlessly with zero errors (the live-device cell guards
itself); `notebooks/*.ipynb` is executed by `testing/python/test_notebook.py`.

Regenerate:  python notebooks/build_feature_tutorial.py
"""
import pathlib

import nbformat as nbf

nb = nbf.v4.new_notebook()
cells = []


def md(s):
    cells.append(nbf.v4.new_markdown_cell(s))


def code(s):
    cells.append(nbf.v4.new_code_cell(s))


# ============================================================ intro
md(r"""# aiudio — Full-Feature Tutorial

A guided, **plotted** tour of the aiudio audio pipeline at the end of **Phase 0** — every
relevant **mode of operation** and **feature**, building intuition with figures as we go.

**The mental model (30 seconds).** aiudio is an AI-native audio framework: a single typed
**signal graph** (the *IR*) that a `GraphExecutor` compiles to a real-time-safe schedule and
runs under *any* clock — a numpy pump, a WAV file, a live device, or a mock — **bit-identically**.
The real-time core is **C++**; **Python is the frontend** (you build, run, edit, and monitor
graphs from Python, which never touches the audio thread). DSP nodes (and, later, neural ones)
are peers under one node contract.

**What this notebook covers**

| | |
|---|---|
| **Modes** | numpy blocks · offline WAV render · live device (macOS) · headless mock · multi-source manager · cross-clock |
| **Features** | the full node library (generators, EQ, dynamics, delay, saturation, pan/width, routing) · live control · graph edit/introspect · boundary DSP (resample / drift / channel-map) |

**Setup:** `pip install . jupyter matplotlib` (the C++ core builds via scikit-build-core).
Companion prose reference: [`docs/80`](../docs/80-pipeline-capabilities.md).""")

code(r"""%matplotlib inline
import numpy as np
import matplotlib.pyplot as plt
import aiudio as a

SR = 48000.0
print("aiudio", a.__version__, "| public API:", len(a.__all__), "names")

# --- small helpers reused throughout -------------------------------------------------------
# render1: build source -> <node> -> sink, run one block of x (channels, frames), return output.
def render1(add_fn, x, channels=1):
    g = a.Graph(); s = g.add_source(); nd = add_fn(g); k = g.add_sink()
    g.connect(s, 0, nd, 0); g.connect(nd, 0, k, 0)
    ex = a.GraphExecutor(); ex.compile(g, channels=channels, sample_rate=SR, max_block=x.shape[1])
    return ex.process(np.ascontiguousarray(x, dtype=np.float32))

# node_response: magnitude frequency response (dB) of a 1-in/1-out node, from its impulse response.
def node_response(add_fn, n=8192):
    imp = np.zeros((1, n), np.float32); imp[0, 0] = 1.0
    h = render1(add_fn, imp)[0]
    H = np.fft.rfft(h)
    return np.fft.rfftfreq(n, 1 / SR), 20 * np.log10(np.maximum(np.abs(H), 1e-9))

# spectrum_db: windowed magnitude spectrum (dB) of a 1-D signal.
def spectrum_db(x, n=8192):
    x = x[:n] * np.hanning(min(len(x), n))
    X = np.fft.rfft(x, n)
    return np.fft.rfftfreq(n, 1 / SR), 20 * np.log10(np.maximum(np.abs(X), 1e-9))

# peak_env: per-window peak envelope, for plotting dynamics.
def peak_env(x, win=128):
    m = len(x) // win
    return np.arange(m) * win / SR, np.array([np.max(np.abs(x[i*win:(i+1)*win])) for i in range(m)])

print("helpers ready: render1, node_response, spectrum_db, peak_env")""")

# ============================================================ 1. first graph
md(r"""## 1. Your first graph (numpy mode)

The simplest **mode of operation**: build a graph, compile it, and push **numpy** blocks
through it. Audio is **planar float32** with shape `(channels, frames)`. A `SourceNode` is the
external input, a `SinkNode` the output; nodes are wired `connect(src, out_port, dst, in_port)`.

Here `source → gain(0.4) → sink` scales a 200 Hz tone.""")
code(r"""g = a.Graph()
src  = g.add_source()
gain = g.add_gain(0.4)
sink = g.add_sink()
g.connect(src, 0, gain, 0)
g.connect(gain, 0, sink, 0)
print("valid:", g.validate())

ex = a.GraphExecutor()
ex.compile(g, channels=1, sample_rate=SR, max_block=1024)
t = np.arange(1024) / SR
tone = np.sin(2 * np.pi * 200 * t).astype(np.float32)[None, :]
out = ex.process(tone)

plt.figure(figsize=(9, 2.6))
plt.plot(t[:480], tone[0, :480], label="in (200 Hz)")
plt.plot(t[:480], out[0, :480], label="out (×0.4)")
plt.legend(loc="upper right"); plt.xlabel("time (s)"); plt.title("source → gain(0.4) → sink")
plt.tight_layout(); plt.show()""")

# ============================================================ 2. node library
md(r"""## 2. The node library — with pictures

The Phase-0 palette is a music-production starter set. We'll *see* what each node does.

### 2.1 Generators — oscillator & noise
`add_oscillator(waveform, freq, amp)` is a 0-input source of signal (sine/saw/square/triangle);
`add_noise(color, amp)` makes white or pink noise.""")
code(r"""def osc(wave):
    g = a.Graph(); o = g.add_oscillator(wave, 220.0, 0.8); k = g.add_sink()
    g.connect(o, 0, k, 0)
    ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)
    return ex.process(np.zeros((1, 512), np.float32))[0]

fig, ax = plt.subplots(1, 4, figsize=(11, 2.4))
for i, w in enumerate(["sine", "saw", "square", "triangle"]):
    ax[i].plot(osc(w)[:440]); ax[i].set_title(w); ax[i].set_yticks([-1, 0, 1])
fig.suptitle("OscillatorNode waveforms (220 Hz)"); plt.tight_layout(); plt.show()""")
code(r"""# White vs pink noise: pink rolls off ~3 dB/octave (1/f). Average several spectra to smooth.
def noise_spectrum(color):
    g = a.Graph(); n = g.add_noise(color, 0.9); k = g.add_sink(); g.connect(n, 0, k, 0)
    ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=8192)
    acc = 0
    for _ in range(20):
        f, db = spectrum_db(ex.process(np.zeros((1, 8192), np.float32))[0])
        acc = acc + 10 ** (db / 10)
    return f, 10 * np.log10(acc / 20)

plt.figure(figsize=(9, 2.8))
for c in ("white", "pink"):
    f, db = noise_spectrum(c); plt.semilogx(f[1:], db[1:] - db[1:].max(), label=c)
plt.legend(); plt.xlabel("Hz"); plt.ylabel("dB (norm)"); plt.title("NoiseNode: white vs pink")
plt.xlim(20, SR/2); plt.tight_layout(); plt.show()""")

md(r"""### 2.2 Filters & EQ
The `BiquadNode` family: low-/high-pass, peaking, and low-/high-shelf. Chain bands, or use
`add_parametric_eq([...])` to get a whole multi-band EQ in one node. We plot the **magnitude
frequency response** (impulse → FFT) of each.""")
code(r"""plt.figure(figsize=(11, 3.0))
plt.subplot(1, 2, 1)
for name, fn in [("low-pass 1k", lambda g: g.add_biquad_lowpass(1000, 0.707, SR)),
                 ("high-pass 1k", lambda g: g.add_biquad_highpass(1000, 0.707, SR)),
                 ("peaking 2k +9", lambda g: g.add_biquad_peaking(2000, 2.0, 9.0, SR))]:
    f, db = node_response(fn); plt.semilogx(f[1:], db[1:], label=name)
plt.legend(fontsize=8); plt.title("single biquads"); plt.xlim(20, SR/2); plt.ylim(-40, 15)
plt.xlabel("Hz"); plt.ylabel("dB")

plt.subplot(1, 2, 2)
eq = lambda g: g.add_parametric_eq([("lowshelf", 120, 0.7, -4.0),
                                    ("peaking", 1000, 1.5, 6.0),
                                    ("peaking", 4000, 2.0, -5.0),
                                    ("highshelf", 9000, 0.7, 4.0)], SR)
f, db = node_response(eq); plt.semilogx(f[1:], db[1:], color="crimson")
plt.title("4-band parametric EQ (one node)"); plt.xlim(20, SR/2); plt.ylim(-12, 12)
plt.axhline(0, color="0.7", lw=0.8); plt.xlabel("Hz"); plt.ylabel("dB")
plt.tight_layout(); plt.show()""")

md(r"""### 2.3 Dynamics — compressor & gate
A `CompressorNode` turns down signal above a threshold; a `GateNode` turns down signal *below*
one. Here we feed a tone whose level **steps from −24 dBFS to −3 dBFS** and plot the input vs
output envelope: the compressor pulls the loud half down toward the threshold.""")
code(r"""t = np.arange(int(SR)) / SR
sig = np.sin(2 * np.pi * 300 * t).astype(np.float32)
sig[: len(sig)//2] *= 10 ** (-24/20)      # quiet half
sig[len(sig)//2 :] *= 10 ** (-3/20)        # loud half

g = a.Graph(); s = g.add_source(); cmp = g.add_compressor(-18.0, 4.0, 5.0, 80.0); k = g.add_sink()
g.connect(s, 0, cmp, 0); g.connect(cmp, 0, k, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)
out = np.concatenate([ex.process(sig[i:i+512][None, :])[0] for i in range(0, len(sig)-512, 512)])

plt.figure(figsize=(9, 2.8))
te, ein = peak_env(sig[:len(out)]); _, eout = peak_env(out)
plt.plot(te, 20*np.log10(np.maximum(ein, 1e-4)), label="in")
plt.plot(te, 20*np.log10(np.maximum(eout, 1e-4)), label="out (compressed)")
plt.axhline(-18, color="0.6", ls="--", lw=0.8, label="threshold -18 dB")
plt.legend(); plt.xlabel("time (s)"); plt.ylabel("dBFS"); plt.title("CompressorNode (4:1)")
plt.tight_layout(); plt.show()""")

md(r"""### 2.4 Saturation — the waveshaper
`add_waveshaper("tanh", drive, mix)` is a smooth nonlinearity: its **transfer curve** soft-clips,
and driving a sine through it adds **harmonics** (a tanh adds odd harmonics).""")
code(r"""ramp = np.linspace(-3, 3, 1024, dtype=np.float32)[None, :]
curve = render1(lambda g: g.add_waveshaper("tanh", 1.0, 1.0), ramp)[0]
tone = np.sin(2*np.pi*1000*np.arange(8192)/SR).astype(np.float32)[None, :] * 0.9
driven = render1(lambda g: g.add_waveshaper("tanh", 3.0, 1.0), tone)[0]

fig, ax = plt.subplots(1, 2, figsize=(11, 3.0))
ax[0].plot(ramp[0], curve); ax[0].plot(ramp[0], ramp[0], "0.7", lw=0.8)
ax[0].set_title("transfer curve y=tanh(x)"); ax[0].set_xlabel("in"); ax[0].set_ylabel("out")
f, db = spectrum_db(driven); ax[1].plot(f, db - db.max())
ax[1].set_title("1 kHz × drive 3 → harmonics"); ax[1].set_xlim(0, 8000); ax[1].set_ylim(-80, 2)
ax[1].set_xlabel("Hz"); ax[1].set_ylabel("dB")
plt.tight_layout(); plt.show()""")

md(r"""### 2.5 Time & space — delay, pan, stereo width
`add_delay` is an echo with feedback; `add_pan` places a mono source in the stereo field
(equal-power); `add_stereo_width` widens/narrows via mid/side.""")
code(r"""# delay: an impulse + 0.5 feedback → decaying echoes every 4800 frames (0.1 s)
imp = np.zeros((1, 24000), np.float32); imp[0, 0] = 1.0
echoes = render1(lambda g: g.add_delay(1.0, 4800, 0.5, 0.6, 1), imp)[0]
# pan: sweep -1..+1 and measure L/R level (the equal-power law)
pans = np.linspace(-1, 1, 41); L, R = [], []
for p in pans:
    # feed a 2-ch input so the numpy boundary keeps the pan's stereo output (the pan reads ch 0)
    g = a.Graph(); s = g.add_source(); pn = g.add_pan(float(p)); k = g.add_sink()
    g.connect(s, 0, pn, 0); g.connect(pn, 0, k, 0)
    ex = a.GraphExecutor(); ex.compile(g, channels=2, sample_rate=SR, max_block=256)
    y = ex.process(np.ones((2, 256), np.float32))
    L.append(np.sqrt((y[0]**2).mean())); R.append(np.sqrt((y[1]**2).mean()))

fig, ax = plt.subplots(1, 2, figsize=(11, 2.8))
ax[0].plot(np.arange(len(echoes))/SR, echoes); ax[0].set_title("DelayNode: 0.1 s, 50% feedback")
ax[0].set_xlabel("time (s)")
ax[1].plot(pans, L, label="L"); ax[1].plot(pans, R, label="R")
ax[1].set_title("PanNode (equal-power)"); ax[1].set_xlabel("pan"); ax[1].set_ylabel("RMS"); ax[1].legend()
plt.tight_layout(); plt.show()""")

md(r"""### 2.6 Routing, channels & utilities
`add_mixer` (per-input gains), `add_channel_matrix` (a routing matrix), `add_downmix`/`add_upmix`
(width change), and `add_dc_blocker` (removes a DC offset). Here a DC-offset tone is cleaned.""")
code(r"""t = np.arange(2048) / SR
dc_tone = (0.5 + 0.4 * np.sin(2*np.pi*200*t)).astype(np.float32)[None, :]   # +0.5 DC offset
cleaned = render1(lambda g: g.add_dc_blocker(20.0, 1), dc_tone)[0]
plt.figure(figsize=(9, 2.4))
plt.plot(t, dc_tone[0], label="in (+0.5 DC)"); plt.plot(t, cleaned, label="dc_blocker out")
plt.axhline(0, color="0.7", lw=0.8); plt.legend(); plt.xlabel("time (s)")
plt.title("DcBlockerNode removes the offset (settles to 0-mean)"); plt.tight_layout(); plt.show()

# channel matrix: swap L<->R, verify numerically
g = a.Graph(); s = g.add_source(); cm = g.add_channel_matrix(2, 2); k = g.add_sink()
g.connect(s, 0, cm, 0); g.connect(cm, 0, k, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=2, sample_rate=SR, max_block=8)
for o, i, v in [(0,0,0.0),(0,1,1.0),(1,0,1.0),(1,1,0.0)]: ex.set_param(cm, o*2+i, v)
sw = ex.process(np.stack([np.full(8,0.2,np.float32), np.full(8,0.8,np.float32)]))
print(f"channel_matrix swap: in (0.2, 0.8) -> out ({sw[0,0]:.1f}, {sw[1,0]:.1f})")""")

# ============================================================ 3. run + control
md(r"""## 3. Compiling, telemetry & the **live control plane**

`compile()` turns the graph into a static schedule; `process()` runs a block. The executor
exposes telemetry (`render_count`, `xrun_count`, `latency_frames`, …). While the engine runs,
parameters are changed by **enqueuing** edits on a lock-free queue — `set_gain`/`set_cutoff`/
`set_q`/`set_param` — applied at the next block, never blocking the audio thread.

Below we hold a low-pass on a fixed white-noise input and **sweep the cutoff live**, plotting
the output spectrum at each setting — the filter moves with no recompile.""")
code(r"""rng = np.random.default_rng(0)
noise = rng.standard_normal((1, 8192)).astype(np.float32) * 0.2
g = a.Graph(); s = g.add_source(); lp = g.add_biquad_lowpass(1000, 0.707, SR); k = g.add_sink()
g.connect(s, 0, lp, 0); g.connect(lp, 0, k, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=8192)

plt.figure(figsize=(9, 3.0))
for fc in (300, 1000, 4000, 12000):
    ex.set_cutoff(lp, float(fc))
    for _ in range(4): y = ex.process(noise)      # let the filter settle at the new cutoff
    f, db = spectrum_db(y[0]); plt.semilogx(f[1:], db[1:], label=f"{fc} Hz", lw=1)
plt.legend(title="live cutoff"); plt.xlim(50, SR/2); plt.xlabel("Hz"); plt.ylabel("dB")
plt.title("set_cutoff() live — filtered white noise"); plt.tight_layout(); plt.show()
print("telemetry:  render_count =", ex.render_count, " latency_frames =", ex.latency_frames,
      " xrun_count =", ex.xrun_count)""")

# ============================================================ 4. modes of operation
md(r"""## 4. Modes of operation

The **same compiled graph** runs under different clocks. We've used the **numpy** pump above.

### 4.1 Offline file rendering
`OfflineBackend(in_wav, out_wav)` reads a WAV, runs the graph faster than real time, and writes
the result. Here we render a tone+noise mix through a high-pass + compressor and compare spectra.""")
code(r"""import wave, tempfile, os
tmp = tempfile.mkdtemp(); in_wav, out_wav = os.path.join(tmp, "in.wav"), os.path.join(tmp, "out.wav")
tt = np.arange(int(SR)) / SR
src_sig = (0.3*np.sin(2*np.pi*150*tt) + 0.1*rng.standard_normal(len(tt))).astype(np.float32)
with wave.open(in_wav, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(SR))
    w.writeframes((np.clip(src_sig, -1, 1) * 32767).astype("<i2").tobytes())

g = a.Graph(); s = g.add_source(); hp = g.add_biquad_highpass(120, 0.707, SR)
cmp = g.add_compressor(-20, 3, 5, 80); k = g.add_sink()
for x, y in [(s, hp), (hp, cmp), (cmp, k)]: g.connect(x, 0, y, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=1024)
ob = a.OfflineBackend(in_wav, out_wav, a.WavFormat.Int16); ob.open(ex, block_size=512); ob.start()
with wave.open(out_wav, "rb") as r:
    rendered = np.frombuffer(r.readframes(r.getnframes()), "<i2").astype(np.float32) / 32768.0
print("frames_rendered:", ob.frames_rendered)

plt.figure(figsize=(9, 2.8))
f, dbi = spectrum_db(src_sig); _, dbo = spectrum_db(rendered)
plt.semilogx(f[1:], dbi[1:], label="input", alpha=0.7)
plt.semilogx(f[1:], dbo[1:], label="rendered (HP + comp)", alpha=0.9)
plt.legend(); plt.xlim(20, SR/2); plt.xlabel("Hz"); plt.ylabel("dB")
plt.title("OfflineBackend WAV render"); plt.tight_layout(); plt.show()

try:
    from IPython.display import Audio, display
    display(Audio(rendered, rate=int(SR)))      # playback widget (interactive only)
except Exception as e:   # noqa: BLE001
    print("(audio widget unavailable here:", e, ")")""")

md(r"""### 4.2 Live device (macOS) & 4.3 the headless mock

A **live device** backend (`DeviceBackend`/`InputBackend`/`DuplexBackend`/`TapBackend`) drives
the *same* executor from a Core Audio IOProc — a pure-C++ audio thread. The cell self-guards
(skips where there's no device / non-macOS). The **`MockBackend`** is a deterministic,
manually-ticked stand-in for that clock, so the whole live path runs in CI with no hardware —
here a `MasterClockAdapter` feeds a device's block → graph → back to the device.""")
code(r"""# Headless live path (always runs): device → adapter → manager → graph → device.
g = a.Graph(); s = g.add_source(0); gn = g.add_gain(0.5); k = g.add_sink(0)
g.connect(s, 0, gn, 0); g.connect(gn, 0, k, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=64)
mgr = a.MultiSourceManager(1, 1, 1, 64, 256)
adapter = a.MasterClockAdapter(mgr, ex, 0, 0)
dev = a.MockBackend(); dev.open(adapter, in_channels=1, out_channels=1, block_size=64)
dev.set_input_value(0.5); dev.start(); dev.tick(64)
print(f"mock device: in 0.5 → gain 0.5 → out {dev.captured_output(0):.3f};  "
      f"hot-unplug → inject_disconnect()")
dev.inject_disconnect(); print("  disconnected =", dev.disconnected, "| running =", dev.running)

# Live device — enumerate + guarded audible start (skips cleanly without a device).
if hasattr(a, "DeviceBackend"):
    be = a.DeviceBackend(); devs = be.enumerate()
    print(f"\n{len(devs)} Core Audio device(s); default out:",
          next((d.name for d in devs if d.is_default_output), "—"))
    outs = [d for d in devs if d.output_channels > 0]
    if outs:
        gD = a.Graph(); o = gD.add_oscillator("sine", 220.0, 0.2); kD = gD.add_sink()
        gD.connect(o, 0, kD, 0)
        exD = a.GraphExecutor(); exD.compile(gD, channels=2, sample_rate=SR, max_block=512)
        try:
            if be.open(exD, channels=2, sample_rate=SR, block_size=512,
                       xrun_policy=a.XrunPolicy.BestEffort):
                import time; be.start(); time.sleep(0.3)
                print("  live: running =", be.running, " latency_frames =", be.latency_frames,
                      " (an audible 220 Hz tone)")
                be.stop()
        except Exception as e:   # noqa: BLE001
            print("  live start skipped:", e)
else:
    print("\n(DeviceBackend is macOS-only — headless mock above covers the live path everywhere)")""")

# ============================================================ 5. multi-source & multi-clock
md(r"""## 5. Multi-source & multi-clock

### 5.1 Multi-stream graph (G10)
A graph can read **N input streams** and drive **M output streams** (`add_source(stream=k)` /
`process_multi`). Here stream 0 is a 300 Hz tone and stream 1 is pink noise; a `SumNode` mixes
them — the output spectrum shows both.""")
code(r"""g = a.Graph()
a0, a1 = g.add_source(0), g.add_source(1)
mix, out = g.add_sum(2), g.add_sink(0)
g.connect(a0, 0, mix, 0); g.connect(a1, 0, mix, 1); g.connect(mix, 0, out, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=8192)

tt = np.arange(8192) / SR
tone = (0.5 * np.sin(2*np.pi*300*tt)).astype(np.float32)[None, :]
pink = render1(lambda gg: gg.add_noise("pink", 0.6), np.zeros((1, 8192), np.float32))  # reuse noise node
[mixed] = ex.process_multi([tone, pink])
f, db = spectrum_db(mixed[0])
plt.figure(figsize=(9, 2.8)); plt.semilogx(f[1:], db[1:])
plt.axvline(300, color="crimson", ls="--", lw=0.8, label="300 Hz tone")
plt.legend(); plt.xlim(20, SR/2); plt.xlabel("Hz"); plt.ylabel("dB")
plt.title("process_multi: tone (stream 0) + pink noise (stream 1)"); plt.tight_layout(); plt.show()""")

md(r"""### 5.2 The multi-source manager (M10)
`MultiSourceManager` composes **N sources + M sinks on one clock**, each crossing the thread
boundary on its own lock-free ring: producers `push_input`, the pump `process`-es the graph,
consumers `pop_output` — with per-stream xrun telemetry.""")
code(r"""g = a.Graph()
a0, a1 = g.add_source(0), g.add_source(1); mix, out = g.add_sum(2), g.add_sink(0)
g.connect(a0, 0, mix, 0); g.connect(a1, 0, mix, 1); g.connect(mix, 0, out, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=128)
mgr = a.MultiSourceManager(num_inputs=2, num_outputs=1, channels=1, max_block=128, ring_frames=512)

tt = np.arange(128) / SR
collected = []
for blk in range(8):
    base = blk * 128
    s0 = (0.5*np.sin(2*np.pi*440*(np.arange(base, base+128)/SR))).astype(np.float32)[None, :]
    s1 = (0.3*np.sin(2*np.pi*110*(np.arange(base, base+128)/SR))).astype(np.float32)[None, :]
    mgr.push_input(0, s0); mgr.push_input(1, s1)
    mgr.process(ex, 128)
    collected.append(mgr.pop_output(0, 128)[0])
mixed = np.concatenate(collected)
print("source A 440 Hz + source B 110 Hz, mixed by the manager; underruns:",
      mgr.input_underruns(0), mgr.input_underruns(1))
plt.figure(figsize=(9, 2.2)); plt.plot(mixed[:512])
plt.title("MultiSourceManager: two sources mixed on one clock"); plt.xlabel("frame"); plt.tight_layout(); plt.show()""")

md(r"""### 5.3 Cross-clock multi-device (M9.6)
A `CrossClockBridge` syncs an input device + an output device on **separate clocks**: the
off-clock output pulls through a drift-compensated resampler whose servo keeps the ring bounded.
We simulate the output clock running **0.2 % slow** and watch the corrected **ratio** climb and
the ring **fill** stay bounded.""")
code(r"""g = a.Graph(); s = g.add_source(0); gn = g.add_gain(0.5); k = g.add_sink(0)
g.connect(s, 0, gn, 0); g.connect(gn, 0, k, 0)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=64)
mgr = a.MultiSourceManager(1, 1, 1, 64, 512)
bridge = a.CrossClockBridge(mgr, ex, 0, 0, SR, SR, 1, 8192, 64)
din, dout = a.MockBackend(), a.MockBackend()
bridge.attach_master(din, in_channels=1, out_channels=0, block_size=64)
bridge.attach_output(dout, in_channels=0, out_channels=1, block_size=64)
din.set_input_value(0.6); din.start(); dout.start()

acc, ratios, fills = 0.0, [], []
for step in range(12000):
    din.tick(64)
    acc += 64 * 0.998                       # output device clock 0.2% slow
    while acc >= 64: dout.tick(64); acc -= 64
    if step % 100 == 0: ratios.append(bridge.output_ratio); fills.append(bridge.output_fill)

fig, ax = plt.subplots(1, 2, figsize=(11, 2.6))
ax[0].plot(ratios); ax[0].axhline(1.0, color="0.7", lw=0.8); ax[0].set_title("drift ratio (servo)")
ax[0].set_xlabel("×100 blocks")
ax[1].plot(fills); ax[1].set_title("ring fill (bounded)"); ax[1].set_xlabel("×100 blocks"); ax[1].set_ylabel("frames")
plt.tight_layout(); plt.show()
print(f"output={dout.captured_output(0):.3f} (0.6×0.5=0.3 across clocks)  overruns={bridge.output_overruns}")""")

# ============================================================ 6. boundary DSP
md(r"""## 6. Boundary DSP — resampling, drift, channel mapping

The graph is single-rate, so sample-rate/channel conversion lives at the **I/O boundary**.
`Resampler` converts rates (here 44.1 kHz → 48 kHz: more samples, **same pitch**). `map_channels`
maps device↔graph layouts (mono↔stereo / N↔M).""")
code(r"""rs = a.Resampler(channels=1, ratio=44100.0/48000.0)
n_in = 2205                                  # 0.05 s @ 44.1k
x = np.sin(2*np.pi*1000*np.arange(n_in)/44100.0).astype(np.float32)[None, :]
y, consumed = rs.process(np.ascontiguousarray(x), out_cap=int(n_in/rs.ratio)+8)
print(f"in {n_in} @44.1k -> out {y.shape[1]} @48k (≈{n_in*48000//44100})  latency_frames={rs.latency_frames}")

fig, ax = plt.subplots(1, 2, figsize=(11, 2.4))
ax[0].plot(x[0, :160], ".-", ms=3); ax[0].set_title("1 kHz @ 44.1 kHz (in)")
ax[1].plot(y[0, :174], ".-", ms=3); ax[1].set_title("1 kHz @ 48 kHz (resampled — same pitch)")
plt.tight_layout(); plt.show()

mono = np.full((1, 4), 0.6, np.float32)
print("map_channels mono→stereo:", a.map_channels(mono, 2)[:, 0],
      "| stereo→mono avg:", float(a.map_channels(np.stack([np.full(4,1.0,np.float32),
                                                            np.full(4,0.4,np.float32)]), 1)[0, 0]))""")

# ============================================================ 7. edit / introspect
md(r"""## 7. Editing & inspecting the graph

Read the IR back (`nodes()`, `edges()`, `node_type()`) and edit it (`disconnect`, `remove_node`);
removing **tombstones** the slot so other node ids never shift. Recompile to apply.""")
code(r"""g = a.Graph()
s, atten, boost, k = g.add_source(), g.add_gain(0.5), g.add_gain(2.0), g.add_sink()
g.connect(s, 0, atten, 0); g.connect(atten, 0, boost, 0); g.connect(boost, 0, k, 0)
print("before:", g.nodes())
print("edges :", g.edges())
g.remove_node(atten); g.connect(s, 0, boost, 0)      # drop the attenuator, rewire around it
print("after :", "node_type(atten) =", g.node_type(atten), "| live_node_count =", g.live_node_count)
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=16)
print("recompiled, source → boost(2.0):", float(ex.process(np.ones((1, 16), np.float32))[0, 0]))""")

# ============================================================ 8. determinism
md(r"""## 8. One IR, many backends — determinism

The headline guarantee: the **same compiled graph** gives the **same output** on the numpy
pump and the offline file backend (bit-identical within the int16 quantum).""")
code(r"""g = a.Graph(); s = g.add_source(); lp = g.add_biquad_lowpass(800, 0.707, SR); k = g.add_sink()
g.connect(s, 0, lp, 0); g.connect(lp, 0, k, 0)
exN = a.GraphExecutor(); exN.compile(g, channels=1, sample_rate=SR, max_block=512)
ref = np.zeros(len(src_sig), np.float32)
for i in range(0, len(src_sig), 512):
    b = src_sig[i:i+512][None, :]; ref[i:i+b.shape[1]] = exN.process(b)[0]
# offline render of the same filter
ob2 = a.OfflineBackend(in_wav, os.path.join(tmp, "lp.wav"), a.WavFormat.Int16)
exO = a.GraphExecutor(); exO.compile(g, channels=1, sample_rate=SR, max_block=512)
ob2.open(exO, block_size=512); ob2.start()
with wave.open(os.path.join(tmp, "lp.wav"), "rb") as r:
    off = np.frombuffer(r.readframes(r.getnframes()), "<i2").astype(np.float32) / 32768.0
n = min(len(ref), len(off))
print(f"max |numpy - offline| = {np.max(np.abs(ref[:n]-off[:n])):.2e}  (int16 quantum {1/32768:.2e}) → identical")""")

# ============================================================ 9. capstone
md(r"""## 9. Capstone — a complete pipeline

A small **synth + mix**: saw oscillator → high-pass → 3-band EQ → compressor → meter, rendered
to a WAV. Generation + the node library + metering + file output, from one graph — the same one
that would run live on a device or under the multi-source manager, bit-identically.""")
code(r"""g = a.Graph()
osc = g.add_oscillator("saw", 110.0, 0.5)
hp  = g.add_biquad_highpass(70.0, 0.707, SR)
eq  = g.add_parametric_eq([("peaking", 800, 1.0, 4.0), ("highshelf", 8000, 0.7, 3.0)], SR)
cmp = g.add_compressor(-16.0, 4.0, 5.0, 80.0)
mtr, k = g.add_meter(), g.add_sink()
for x, y in [(osc, hp), (hp, eq), (eq, cmp), (cmp, mtr), (mtr, k)]: g.connect(x, 0, y, 0)
assert g.validate()[0]

cap_in, cap_out = os.path.join(tmp, "cap_in.wav"), os.path.join(tmp, "cap_out.wav")
with wave.open(cap_in, "wb") as w:           # 0.5 s placeholder; the oscillator generates
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(SR))
    w.writeframes(np.zeros(int(0.5*SR), "<i2").tobytes())
ex = a.GraphExecutor(); ex.compile(g, channels=1, sample_rate=SR, max_block=512)
ob = a.OfflineBackend(cap_in, cap_out, a.WavFormat.Int16); ob.open(ex, block_size=512); ob.start()
with wave.open(cap_out, "rb") as r:
    synth = np.frombuffer(r.readframes(r.getnframes()), "<i2").astype(np.float32) / 32768.0
print("pipeline:", " → ".join(t for _, t, _, _ in g.nodes()))
print(f"rendered {ob.frames_rendered} frames, peak {np.max(np.abs(synth)):.3f}, "
      f"meter_ms {g.meter_mean_square(mtr):.4f}")

plt.figure(figsize=(9, 2.2)); plt.plot(synth[:1000]); plt.title("capstone synth (first 1000 frames)")
plt.xlabel("frame"); plt.tight_layout(); plt.show()
try:
    from IPython.display import Audio, display; display(Audio(synth, rate=int(SR)))
except Exception:   # noqa: BLE001
    pass""")

md(r"""## Recap

You touched every Phase-0 mode and feature:

- **Modes:** numpy blocks · offline WAV render · live device (macOS, guarded) · headless mock ·
  multi-source manager · cross-clock bridge.
- **Features:** the full node library (generators, EQ family, dynamics, delay, saturation,
  pan/width, routing/channel nodes, meter), the live control plane, graph edit/introspection,
  and the boundary DSP utilities (resampler, drift servo, channel map).

**Not yet (Phases 1–2):** differentiable/trainable execution and neural nodes, the LLM agent,
and Tier-2/3 nodes (reverb, spectral/FFT, loudness, convolution). See the
[roadmap](../README.md#roadmap), [`docs/78`](../docs/78-node-library-roadmap.md) (node tiers),
and [`docs/80`](../docs/80-pipeline-capabilities.md) (capabilities reference). The terse
feature+shortcomings checklist lives in `testing/notebooks/aiudio_acceptance_walkthrough.ipynb`.""")

nb["cells"] = cells
nb["metadata"]["kernelspec"] = {"display_name": "Python 3", "language": "python", "name": "python3"}
nb["metadata"]["language_info"] = {"name": "python"}

out = pathlib.Path(__file__).resolve().parent / "aiudio_feature_tutorial.ipynb"
with open(out, "w") as f:
    nbf.write(nb, f)
print(f"wrote {out} with {len(cells)} cells")

# 71 — I/O Layer Milestone Plan (Input + Output Foundation)

The audio **I/O layer** is the foundation of aiudio: the boundary between the
real world (devices, the plugin host, files) and the graph engine (`50-*`).
Everything — DSP nodes, neural nodes, the agent — sits on top of it. This plan
builds **input and output together**, because they share one contract and one
clock.

> Relationship to other docs: `50-architecture-patterns.md` defines the
> RT-threading model this instantiates; `70-macos-audio-capture-plan.md` details
> the **capture** APIs (its M0–M5 map into **M0/M3/M5** here). This doc is the
> umbrella foundation plan covering **both directions**.
>
> **Provenance:** Core Audio API facts (taps 14.4+, HAL, permissions) were
> web-verified in `70-*`. The abstractions and sequencing below are engineering
> synthesis — confirm exact SDK symbols against headers when implementing.

---

## 1. The one idea that unifies input and output

> **A single duplex callback, driven by a swappable clock.** The engine exposes
> exactly one real-time entry point:
> ```
> process(const AudioBuffer& in, AudioBuffer& out, uint32_t frames, TimeInfo t)
> ```
> Different **backends** call it from different clocks:
> - **Device backend** → the output device's IOProc is the clock (standalone app).
> - **Plugin-host backend** → the host's `processBlock` is the clock (VST3/AU/CLAP).
> - **Offline backend** → a manual pump calls it as fast as possible (render).
>
> Capture sources that run on a *different* clock (process taps, network, Python)
> reconcile via **lock-free ring buffers**. This is the same model JUCE
> (`AudioIODeviceCallback`), PortAudio (callback), and every plugin SDK use — we
> adopt it deliberately rather than invent.

This is why I/O is *one* layer, not two: input and output meet inside `process()`.

```
   ┌─────────── clock source (pick one) ───────────┐
   │  device IOProc │ plugin host │ offline pump    │
   └───────┬─────────────┬─────────────┬────────────┘
           ▼             ▼             ▼
        ┌───────────────────────────────────┐
        │  engine.process(in, out, frames)   │  ← the single RT contract
        └───────────────────────────────────┘
            ▲ in                    │ out ▼
   ring buffers from:        to devices / host / file
   taps, network, Python     + ring buffers to Python/recorder
```

---

## 2. Foundation abstractions (get these interfaces right first)

These are the types both directions share — the actual "foundation."

```cpp
// Non-owning view of deinterleaved float32 audio (RT-safe; no allocation).
struct AudioBuffer {
  float* const* channels;   // channels[ch][frame]
  uint32_t      numChannels;
  uint32_t      numFrames;
};

struct TimeInfo { uint64_t sampleTime; double hostTimeSec; bool clockValid; };

struct AudioDeviceInfo {
  std::string id, name;                 // CoreAudio UID + human name
  uint32_t    inputChannels, outputChannels;
  std::vector<double> sampleRates;
  bool        isDefaultInput, isDefaultOutput;
};

struct StreamConfig {
  std::string inputDeviceId, outputDeviceId;  // either may be empty
  double      sampleRate   = 48000;
  uint32_t    blockSize    = 128;             // frames per callback
  uint32_t    inputChannels = 0, outputChannels = 2;
};

// The single RT entry point the graph executor implements.
struct RenderCallback {
  virtual void process(const AudioBuffer& in, AudioBuffer& out,
                       uint32_t frames, TimeInfo t) noexcept = 0;
};

// A clock/transport. Subclasses: CoreAudioDeviceBackend, CoreAudioTapBackend(in),
// FileBackend, OfflineBackend, PluginHostBackend.
struct AudioBackend {
  virtual std::vector<AudioDeviceInfo> enumerate() = 0;
  virtual bool open(const StreamConfig&, RenderCallback*) = 0;
  virtual bool start() = 0;    // begins calling process() on the RT thread
  virtual void stop()  = 0;
  virtual uint32_t latencyFrames() const = 0;  // for delay compensation
};

// SPSC lock-free ring buffer — the ONLY way audio crosses a thread boundary.
template <class T> class RingBuffer { /* write()/read(), wait-free */ };
```

**Invariants (non-negotiable, from `50-*` §4):** inside `process()` and any device
IOProc — **no allocation, no locks, no syscalls, no Python**. Everything is
pre-sized at `open()`.

---

## 3. Milestones

Rough order-of-magnitude estimates for one developer. Acceptance criteria use
this machine's real devices (default in = **Sennheiser Profile**, default out =
**Kanto ORA4**).

> **Progress:** M0 ✅ implemented (`examples/`, three `sounddevice` spikes) —
> offline `--self-test`s pass and `--list-devices` confirms the Core Audio
> devices; the live audio/mic check is run by the developer on real hardware.
> M1 ✅ implemented — the `aiudio-io` C++ library (CMake + CTest): `AudioBuffer`,
> `StreamConfig`, `RenderCallback`/`AudioBackend` contracts, the lock-free SPSC
> `RingBuffer`, and sample-format conversions. The SPSC stress test (1M items, one
> producer/one consumer) is clean under **ThreadSanitizer** and **ASan/UBSan**.
> Runnable usage examples in `examples/cpp/`.
> M2 🔜 in review (PR) — the **Core Audio HAL output backend** (`CoreAudioBackend`,
> macOS): device `enumerate()`, output IOProc driving a `RenderCallback`, sample
> rate / buffer-size negotiation, interleaved + non-interleaved output handling.
> Builds warning-free; `enumerate()` verified on macOS 26 (matches the known
> devices, finds the default output) via `test_coreaudio_enumerate`; live tone
> playback (`ex_play_sine_device`) is the developer's on-device check. M3–M9 pending.

### Phase 0 — Prove the plumbing (both directions)

| M | Deliverable | Acceptance criteria | Est. |
|---|---|---|---|
| **M0** | Python spikes: ① sine→output, ② input→meter, ③ input→output **passthrough** (full duplex) via `sounddevice` | Hear a 440 Hz tone from Kanto; see live RMS from Sennheiser mic; talk→hear yourself with no audible dropouts at 48 kHz/128 | 0.5 d |

> M0 exists only to validate permissions + device plumbing before any C++. Throw
> it away after.

### Phase 1 — Core contracts

| M | Deliverable | Acceptance criteria | Est. |
|---|---|---|---|
| **M1** | The §2 headers as a CMake C++ lib (`aiudio-io`): `AudioBuffer`, `StreamConfig`, `RenderCallback`, `AudioBackend`, lock-free `RingBuffer<float>` + unit tests | `brew install cmake`; lib builds; RingBuffer passes a 2-thread SPSC stress test (no data loss, wait-free); interleave⇄planar + float conversion helpers tested | 2–3 d |

### Phase 2 — Real-time device I/O (the heartbeat)

| M | Deliverable | Acceptance criteria | Est. |
|---|---|---|---|
| **M2** | **CoreAudio output backend** — HAL output IOProc pulls from a `RenderCallback`; device enumeration; format negotiation | Glitch-free sine + wav playback to a *chosen* device (Kanto / MacBook speakers); switch device without restart; xrun counter = 0 over 60 s | 3–4 d |
| **M3** | **CoreAudio input backend** — HAL input IOProc → ring buffer (= `70-*` M1) | Captures default input glitch-free at 48 kHz/128; `enumerate()` lists all 10 devices; RMS matches M0 | 3–4 d |
| **M4** | **Full-duplex / shared clock** — input+output on one IOProc (or an aggregate device) so live in→process→out runs on a single clock | Round-trip passthrough; **measured round-trip latency reported** (target ≲ 10–15 ms at 128 frames); no input/output drift over 10 min | 3–5 d |

> M2→M4 is the core engine heartbeat. After M4, aiudio can run a real-time graph
> against real hardware.

### Phase 3 — Extended sources & sinks (under the same abstraction)

| M | Deliverable | Acceptance criteria | Est. |
|---|---|---|---|
| **M5** | **Process-tap input source** (= `70-*` M2): system + per-PID capture via tap→aggregate device, behind `AudioBackend`; signed binary w/ `NSAudioCaptureUsageDescription` | Capture whole-system audio **and** one app (e.g. Spotify) with no BlackHole; purple TCC indicator; same `AudioBuffer` contract as M3 | 4–6 d |
| **M6** | **File + Offline backend** — file **in** (decode via AVFoundation/libsndfile) and file **out** (wav/flac encode); an `OfflineBackend` that pumps `process()` faster-than-real-time | Render a file→graph→file offline bit-exact and faster than real-time; same `RenderCallback` runs in RT *and* offline unchanged | 3–4 d |
| **M7** | **Plugin-host backend (design + stub)** — abstraction where the **host owns I/O**; aiudio receives `in`/`out` buffers instead of opening a device | A null/CLAP stub drives `process()` from a host-style callback; documents buffer-size/SR/latency negotiation; no device code on this path | 2–3 d (full impl later) |

### Phase 4 — Bindings, graph integration, hardening

| M | Deliverable | Acceptance criteria | Est. |
|---|---|---|---|
| **M8** | **Python bindings (nanobind)** + graph wiring: `SourceNode`/`SinkNode` in the graph IR (`50-*`); backend drives the RT executor | From Python: `open_duplex(in=Sennheiser, out=Kanto)`, build `source → gain → sink`, hear it live; `open_process_tap("com.spotify.client")` yields numpy blocks in Jupyter | 3–4 d |
| **M9** | **Robustness** — sample-rate conversion, device hot-plug/disconnect, xrun/underrun policy, drift compensation, channel mapping/routing, multi-device | Survives unplugging the USB interface mid-stream (clean fallback, no crash); SR mismatch (44.1↔48) resampled transparently; mono↔stereo mapping correct | ongoing |

---

## 4. Dependency / sequencing graph

```
M0 ─▶ M1 ─▶ M2 ─▶ M3 ─▶ M4 ─┬─▶ M5 (taps in)
                            ├─▶ M6 (file/offline)
                            ├─▶ M7 (plugin host)
                            └─▶ M8 (python + graph) ─▶ M9 (hardening)
```
- **M1 gates everything** — the shared contracts.
- **M2 before M3**: output is the clock; an output-only sine is the simplest
  end-to-end RT test.
- **M4 unlocks live effects** and is the natural "foundation usable" checkpoint.
- **M5/M6/M7 are parallelizable** once M4 lands (independent backends).

---

## 5. Definition of done — "the foundation is built"

The I/O layer is complete when **all of these hold**:

1. **One contract, many clocks:** the exact same `RenderCallback::process()` runs
   unchanged under a **device duplex** backend (RT), an **offline file** backend,
   and a **plugin-host** stub.
2. **Both directions, RT-safe:** capture (device + system + per-app) and playback
   (device + file) work with **zero allocation/locks** on the audio thread;
   verified glitch-free at 48 kHz / 128 frames.
3. **Discoverable & selectable:** `enumerate()` lists every device; any in/out
   pair is openable; format (SR/blocksize/channels) is negotiated, not assumed.
4. **Measured latency:** round-trip latency is reported per backend for delay
   compensation (`AudioBackend::latencyFrames()`).
5. **Crosses threads safely:** every off-clock source/sink (taps, Python,
   recorder) uses the lock-free `RingBuffer`; no audio-thread blocking anywhere.
6. **Drivable from Python and from the graph:** exposed as `SourceNode`/`SinkNode`
   honoring the node contract in `50-*` §2.

At that point, the graph engine, DSP nodes, and neural nodes have a stable floor
to build on — and "capture from any source, process, play to any sink, in
real-time or offline" is a solved, reusable primitive.

---

## 6. Key risks specific to the I/O layer

| Risk | Mitigation |
|---|---|
| **Clock drift** between separate input & output devices | Prefer a single duplex IOProc / aggregate device (M4); add drift compensation (M9) when devices differ |
| **Process-tap TCC/signing** silently fails for CLI/Python | Ship the tap helper as a signed binary/bundle with `NSAudioCaptureUsageDescription` (`70-*` §6) |
| **Block-size / SR mismatch** host vs device vs model | Format negotiation in M2/M3; resampler in M9; never assume 48 k/128 |
| **Plugin vs standalone divergence** | M7 forces the host-owns-I/O path early so the abstraction doesn't ossify around device ownership |
| **Latency creep** from off-thread sources | `latencyFrames()` reported everywhere; the graph compensates (mirrors ANIRA/Neutone delay handling, `30-*`) |

---

## 7. Next action

Start **M0** (½ day, no C++): the three `sounddevice` spikes — sine→Kanto,
Sennheiser→meter, and live passthrough — to confirm permissions + duplex plumbing
end-to-end. Then **M1** (`brew install cmake`; the `aiudio-io` headers + ring
buffer) is the first real, permanent piece of the library.

I can scaffold M0 (the spike scripts) and the M1 CMake skeleton + headers on
request.

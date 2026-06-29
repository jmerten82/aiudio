# examples/cpp — using & testing the `aiudio-io` library (M1)

Runnable C++ examples that demonstrate how to consume the M1 I/O foundation
(`aiudio-io`): the lock-free ring buffer, the `RenderCallback` contract, and the
sample-format conversions. They are teaching code, not part of the library.

## Build

Examples build with the project by default (`-DAIUDIO_BUILD_EXAMPLES=ON`):

```bash
cmake -S . -B build
cmake --build build -j
```

The binaries land in `build/examples/cpp/`.

## The examples

| Binary | Demonstrates | Run |
|---|---|---|
| `ex_ring_buffer` | Moving audio across a thread boundary with the **SPSC `RingBuffer`** (producer thread → consumer), with integrity check | `./build/examples/cpp/ex_ring_buffer` |
| `ex_render_callback` | Implementing **`RenderCallback`** nodes (`SineSource`, `GainNode`) and driving them with the device-free **`OfflineDriver`** pump | `./build/examples/cpp/ex_render_callback` |
| `ex_conversions` | The **device ⇄ engine** format boundary: int16⇄float + interleave⇄planar round trip | `./build/examples/cpp/ex_conversions` |
| `ex_offline_render_wav` | End-to-end: render a node **offline to a playable WAV** | `./build/examples/cpp/ex_offline_render_wav out.wav` then `afplay out.wav` |

Each example prints its results and exits `0` on success, so they double as
smoke tests. `example_support.hpp` holds the shared `SineSource` / `GainNode` /
`OfflineDriver` / `WavWriter` used by a couple of them.

## How to test M1

Two complementary layers:

1. **Unit tests (the formal correctness bar)** — CTest:
   ```bash
   ctest --test-dir build --output-on-failure
   ```
   Covers ring-buffer FIFO/full/empty/bulk + a 1M-item SPSC stress test, and the
   conversion round trips. See `../../tests/`.

2. **Examples as smoke tests** — run the four binaries above; each returns
   non-zero on failure.

### Under sanitizers
The ring buffer is verified race-free; reproduce it yourself:

```bash
cmake -S . -B build-tsan -DAIUDIO_SANITIZE=thread
cmake --build build-tsan -j
./build-tsan/examples/cpp/ex_ring_buffer          # ThreadSanitizer: no data races
ctest --test-dir build-tsan --output-on-failure

cmake -S . -B build-asan -DAIUDIO_SANITIZE=address,undefined
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure   # ASan/UBSan: no memory/UB errors
```

## How to test M2 (Core Audio output backend — macOS)

The M1 examples above use a device-free offline pump. M2 adds the real
`CoreAudioBackend`, tested three ways — from safest to loudest:

| What | Audio? | Run |
|---|---|---|
| **Enumeration unit test** | none | `ctest --test-dir build -R coreaudio` |
| **`ex_device_probe`** — silent, instrumented IOProc | **silent** | `./build/examples/cpp/ex_device_probe --seconds 1.5` |
| **`ex_play_sine_device`** — plays a tone | **makes sound** | `./build/examples/cpp/ex_play_sine_device --device Kanto --seconds 5` |

1. **`test_coreaudio_enumerate`** (CTest, no audio) — asserts the HAL enumeration
   lists devices and finds a system default output. Headless/CI-safe.
2. **`ex_device_probe`** — opens the output device with a **silent** RenderCallback
   that records what the IOProc delivered (callback count, frames/block, channels,
   throughput vs. expected) using RT-safe relaxed atomics. It makes **no sound**,
   so it's an *objective* runtime check you can run anytime. Exits `0` on PASS.
   List devices first with `ex_play_sine_device --list`; target one with
   `--device <name-substring>`.
   <br>Example output (default device = Kanto ORA4): `551 callbacks, 128
   frames/cb, 2 ch, 70528/72000 frames → PASS`.
3. **`ex_play_sine_device`** — the subjective/manual acceptance check: plays a
   sine to a chosen device (the M2 "glitch-free playback" criterion). Use
   headphones; it produces real sound.

## How to test M3 (Core Audio input backend — macOS)

M3 adds `CoreAudioInputBackend` — capture from a device's HAL IOProc, delivered to
a `RenderCallback` as `in`. Because capture needs the **microphone**, these are
**hands-on tests** (macOS prompts for Microphone permission on first run; if
denied you'll get silence). List input devices with `ex_play_sine_device --list`'s
sibling info, or just pass a name substring.

| What | Mic? | Run |
|---|---|---|
| **Enumeration unit test** | none | `ctest --test-dir build -R coreaudio` (also asserts a default **input** exists) |
| **`ex_capture_meter`** — live level meter | **yes** | `./build/examples/cpp/ex_capture_meter --device Sennheiser --seconds 10` |
| **`ex_capture_to_ringbuffer`** — capture → ring buffer → WAV | **yes** | `./build/examples/cpp/ex_capture_to_ringbuffer --device Sennheiser --seconds 5 capture.wav` then `afplay capture.wav` |

1. **`ex_capture_meter`** — opens the input device and prints a live dBFS meter;
   **speak into the mic and watch it move**. At the end it reports IOProc stats
   (callbacks, frames/cb, channels). This is the M3 "captures input" check.
2. **`ex_capture_to_ringbuffer`** — the headline demo: the audio thread (producer)
   mono-mixes each captured block into a lock-free **`RingBuffer<float>`**; the
   main thread (consumer) drains it to a WAV. Exercises the M1 ring buffer across
   a real audio→consumer thread boundary (ADR-0004) and reports any dropped frames
   (ring overrun). Record a few seconds, then `afplay capture.wav` to verify.

> The mic permission attaches to the host (Terminal / the binary). See
> `docs/70-macos-audio-capture-plan.md` §6.

## How to test M4 (full-duplex / shared clock — macOS)

M4 adds `CoreAudioDuplexBackend` — input **and** output on one clock (a single
device, or a drift-compensated **aggregate device** when they differ; ADR-0008).

| What | Sound? | Run |
|---|---|---|
| **`ex_duplex_probe`** — silent objective check | **silent** (needs mic) | `./build/examples/cpp/ex_duplex_probe --seconds 1.5` |
| **`ex_duplex_passthrough`** — live monitoring | **makes sound** | `./build/examples/cpp/ex_duplex_passthrough --seconds 10` |

1. **`ex_duplex_probe`** — opens the duplex backend (default in + default out),
   **outputs silence** while measuring the captured input, and reports the IOProc
   stats (callbacks, frames, in/out channel counts, input level, whether an
   aggregate device was created). No sound; objective PASS/CHECK. On the dev
   machine it builds an aggregate (Sennheiser-in + Kanto-out) and reports
   `in=1/out=2`.
2. **`ex_duplex_passthrough`** — the live "capture → process → playback" check:
   routes the mic to the speakers with a gain. **⚠️ Use headphones** (open
   mic + open speakers = feedback). Confirms low-latency monitoring on one clock.

## How to test M5 (process taps — system / per-app capture, macOS 14.4+)

M5 adds `CoreAudioProcessTapBackend` — capture **output** audio of the whole
system or one process, with no virtual device (ADR-0007).

| What | Permission | Run |
|---|---|---|
| **`ex_list_audio_processes`** — list tappable PIDs | none | `./build/examples/cpp/ex_list_audio_processes` |
| **`ex_tap_capture --system`** — capture all system audio | **audio-recording (purple dot)** | `./build/examples/cpp/ex_tap_capture --system --seconds 5 system.wav` |
| **`ex_tap_capture --pid <PID>`** — capture one app | **audio-recording** | `./build/examples/cpp/ex_tap_capture --pid 1234 --seconds 5 app.wav` |

1. **`ex_list_audio_processes`** — lists the process objects Core Audio exposes
   (PID + bundle id). No permission; safe anytime. Pick a PID from here.
2. **`ex_tap_capture`** — taps system (`--system`) or one process (`--pid N`),
   prints a live dBFS meter of the tapped audio, and optionally records a mono WAV
   (positional path). **Play some audio first.** On first run macOS shows the
   **audio-recording (purple-dot) permission prompt** — grant it, then re-run.
   - This binary **embeds `NSAudioCaptureUsageDescription` and is ad-hoc signed**
     by CMake so the prompt appears. A bare unsigned CLI without that key would
     silently capture nothing — see `docs/70-macos-audio-capture-plan.md` §6.
   - Then `afplay system.wav` to verify.

## How to test G1 (graph IR + node contract — cross-platform, no audio)

G1 adds the `aiudio-graph` library: the `Node` contract, the typed `Graph` IR with
`validate()`, and trivial `GainNode` / `SumNode`. No audio device, no permissions.

| What | Run |
|---|---|
| **Unit tests** | `ctest --test-dir build -R "graph"` (graph validation + node processing) |
| **`ex_build_graph`** | `./build/examples/cpp/ex_build_graph` |

1. **`test_graph`** — `validate()` accepts a DAG and rejects an out-of-range port,
   an input port with two drivers, and a cycle. **`test_graph_nodes`** — `GainNode`
   scales and `SumNode` mixes correctly.
2. **`ex_build_graph`** — builds `GainNode ┐`/`GainNode ┘→ SumNode`, prints the
   graph, runs `validate()` (and shows it rejecting a cycle + a bad port), then
   runs the nodes on a block **by hand** (no executor in G1). Exits `0` on success.

## How to test G2 (graph executor — cross-platform, no audio)

G2 adds the **`GraphExecutor`**: `compile()` turns a graph into a static schedule
with pre-allocated buffers, and `process()` runs it as a `RenderCallback`.

| What | Run |
|---|---|
| **Unit tests** | `ctest --test-dir build -R graph_executor` |
| **`ex_run_graph_offline`** | `./build/examples/cpp/ex_run_graph_offline` |

1. **`test_graph_executor`** — `Source → Gain → Sink` applies gain bit-exact;
   `Source ─┬─► Gain(0.5) ─► Sum ◄─ Gain(0.8) ◄─┘` (fan-out + fan-in) yields 1.3;
   `compile()` rejects an invalid (cyclic) graph. Runs clean under ASan/UBSan.
2. **`ex_run_graph_offline`** — compiles the 5-node fan-out/fan-in graph and drives
   it block-by-block via the executor (an offline pump); prints `out[0] = 1.300`.
   The **same executor** is a `RenderCallback`, so in G3 a Core Audio backend will
   drive it for live audio — no code change to the graph.

## How to test G3 (live graph through a backend — macOS)

G3 wires a compiled graph to the **Core Audio duplex backend** — the first live
`capture → graph → playback`. Adds `MeterNode` (passthrough + level). The
`GraphExecutor` is a `RenderCallback`, so the backend drives the whole graph.

| What | Sound? | Run |
|---|---|---|
| **`ex_graph_capture_probe`** — silent objective check | **silent** (needs mic) | `./build/examples/cpp/ex_graph_capture_probe --seconds 1.5` |
| **`ex_graph_live_passthrough`** — live monitoring | **makes sound** | `./build/examples/cpp/ex_graph_live_passthrough --gain 1.0 --seconds 10` |

1. **`ex_graph_capture_probe`** — `Source → Meter → Gain(0) → Sink` driven by the
   duplex backend; `Gain(0)` silences the output, so it makes **no sound** while
   proving the graph runs live (the MeterNode reports blocks executed + input
   level). Objective PASS/CHECK. (Verified here: 566 blocks, live input present.)
2. **`ex_graph_live_passthrough`** — `Source → Gain → Meter → Sink` live: mic →
   speakers through the graph, with a live meter. **⚠️ Use headphones.** Mono mic
   appears on the left channel (channel mapping/upmix is a later refinement).

## How to test G4 + M6 (node library + offline/file backend — cross-platform, no audio)

G4 adds a real DSP node (`BiquadNode`); M6 adds WAV I/O + the `OfflineBackend`
(renders a graph over a file faster than real time). All offline — no device.

| What | Run |
|---|---|
| **Unit tests** | `ctest --test-dir build -R "biquad\|offline\|wav"` |
| **`ex_render_file_offline`** | `./build/examples/cpp/ex_render_file_offline --cutoff 800` |

1. **`test_wav_file`** — WAV round-trips (float32 bit-exact; int16 within ~1 LSB).
   **`test_biquad_node`** — RBJ lowpass passes DC, highpass blocks DC.
   **`test_offline_render`** — the golden test: `file → Source→Gain(0.5)→Sink →
   file` rendered via `OfflineBackend` is **bit-exact** (float32).
2. **`ex_render_file_offline`** — generates a 300 Hz + 5 kHz input WAV, renders it
   through `Source → Biquad(lowpass) → Gain → Sink` to an output WAV (faster than
   real time), and prints the paths. On macOS, compare by ear:
   `afplay in.wav` vs `afplay out.wav` — the 5 kHz tone is attenuated (verified
   numerically: output RMS 0.40 → 0.25).

> The **same** `GraphExecutor` runs **live** (G3, device backend) and **offline**
> (M6, `OfflineBackend`) unchanged — the ADR-0005 "swappable clock" in action.

## How to test G5 (live graph edits — cross-platform, no audio)

G5 makes `GraphExecutor::compile()` install a new schedule with an **atomic swap**
while `process()` runs — the audio thread never sees a half-edited graph, and the
old schedule is freed off-thread once released (RCU). This is the hook the agent
will use to edit a running graph.

| What | Run |
|---|---|
| **Unit tests** | `ctest --test-dir build -R live_edit` |
| **`ex_graph_live_edit`** | `./build/examples/cpp/ex_graph_live_edit` |
| **Race check (TSan)** | `cmake -S . -B build-tsan -DAIUDIO_SANITIZE=thread && cmake --build build-tsan -j && ctest --test-dir build-tsan -R live_edit` |

1. **`test_graph_live_edit`** — `swap_takes_effect_and_reclaims` (a swapped-in
   graph is live next block; retired schedules are reclaimed, pending set stays
   bounded) and `live_swap_is_race_free` (1000 swaps while a thread spins
   `process()` — no garbage output). **Run it under ThreadSanitizer** to confirm
   race-freedom (verified clean).
2. **`ex_graph_live_edit`** — pumps a graph, **swaps `Gain(0.5)` → `Gain(0.25)`
   mid-stream**, and shows the output change (0.5 → 0.25) with no stop/restart.

## Notes

- M1 examples run **without any audio device** (the `OfflineDriver` pump); the M2
  examples/tests use **Core Audio** and are macOS-only.
- `ex_device_probe` is the recommended quick check that the device path works
  without making noise; `ex_play_sine_device` is the final by-ear confirmation.
- `out.wav` is git-ignored.

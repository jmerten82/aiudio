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

## Notes

- M1 examples run **without any audio device** (the `OfflineDriver` pump); the M2
  examples/tests use **Core Audio** and are macOS-only.
- `ex_device_probe` is the recommended quick check that the device path works
  without making noise; `ex_play_sine_device` is the final by-ear confirmation.
- `out.wav` is git-ignored.

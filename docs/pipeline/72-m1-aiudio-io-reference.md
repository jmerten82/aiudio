# 72 — M1 Reference: what `aiudio-io` implements

A precise, code-grounded description of **exactly** what milestone **M1**
implements — the `aiudio-io` C++ library. This is the *reference* companion to the
*plan* in [`71-io-layer-milestones.md`](71-io-layer-milestones.md) §M1.

> **TL;DR.** M1 is the **contracts and primitives** of the I/O layer — the types,
> interfaces, a lock-free ring buffer, and sample-format conversions — plus its
> CMake build, unit tests, and usage examples. It is **header-and-a-bit-of-`.cpp`
> only**: it does **not** open any audio device, drive Core Audio, or run a graph.
> It is the skeleton every later backend (M2+) and the graph engine build on.

---

## 1. Scope — what M1 *is* and *is not*

| M1 **is** | M1 is **not** (later milestones) |
|---|---|
| The shared value types (`AudioBuffer`, `TimeInfo`, `AudioDeviceInfo`, `StreamConfig`) | A working audio device / Core Audio backend (**M2/M3**) |
| The two core interfaces: `RenderCallback`, `AudioBackend` | Any concrete `AudioBackend` implementation |
| A lock-free **SPSC `RingBuffer<T>`** | The graph engine / node graph (separate track) |
| Sample-format **conversions** (planar⇄interleaved, int16⇄float32) | Process taps, file/offline backends, plugin host (**M5–M7**) |
| CMake build (`aiudio::io`), CTest unit tests, C++ usage examples | Python bindings (**M8**) |

Everything in M1 is **pure, dependency-free C++20** (only the standard library +
pthreads for tests/examples). No platform audio APIs are touched yet.

## 2. File map

```
include/aiudio/io/          (public headers — the library surface)
  audio_buffer.hpp     27 LOC  non-owning planar float32 view
  types.hpp            40 LOC  TimeInfo, AudioDeviceInfo, StreamConfig
  render_callback.hpp  26 LOC  the duplex process() contract
  audio_backend.hpp    39 LOC  swappable clock/transport interface
  ring_buffer.hpp     117 LOC  SPSC wait-free ring buffer  ← the substantive code
  conversions.hpp      27 LOC  conversion declarations
src/io/
  conversions.cpp      39 LOC  conversion definitions (the only compiled TU)
CMakeLists.txt         53 LOC  aiudio::io lib + options + sanitizers
tests/                         CTest unit tests (test_framework.hpp + 2 suites)
examples/cpp/                  4 runnable usage examples + example_support.hpp
```

The library proper is **~275 lines** of headers + one 39-line `.cpp`. It compiles
to a small static lib `libaiudio_io.a` (aliased `aiudio::io`).

---

## 3. The components in detail

### 3.1 `AudioBuffer` (`audio_buffer.hpp`)
A **non-owning view** of deinterleaved (planar) float32 audio — the buffer type
that crosses the `RenderCallback` boundary.

```cpp
struct AudioBuffer {
    float* const* channels;       // channels[ch][frame]
    std::uint32_t numChannels;
    std::uint32_t numFrames;
    float* channel(std::uint32_t c) const noexcept;  // channels[c]
    bool   empty()  const noexcept;                  // 0 channels or 0 frames
};
```
- **Owns nothing, allocates nothing** → safe to construct on the audio thread
  (ADR-0004). The caller owns the actual sample storage.
- **Planar** (one pointer per channel), **float32** — the engine's internal
  format convention (`CLAUDE.md` §8). Devices' interleaved/integer buffers are
  converted at the boundary (§3.6).

### 3.2 Value types (`types.hpp`)
Plain aggregates, no logic:
- **`TimeInfo`** `{ uint64 sampleTime; double hostTimeSeconds; bool clockValid; }`
  — per-block timing handed to `process()`.
- **`AudioDeviceInfo`** `{ id, name, inputChannels, outputChannels,
  sampleRates[], isDefaultInput, isDefaultOutput }` — what a backend's
  `enumerate()` returns (no device is enumerated yet in M1).
- **`StreamConfig`** `{ inputDeviceId, outputDeviceId, sampleRate=48000,
  blockSize=128, inputChannels=0, outputChannels=2 }` — requested stream
  parameters; empty device id ⇒ system default; either channel count may be 0
  (half-duplex).

### 3.3 `RenderCallback` (`render_callback.hpp`) — the one RT entry point
The single duplex contract the graph executor implements and every backend calls
(ADR-0005):

```cpp
class RenderCallback {
public:
    virtual ~RenderCallback() = default;
    virtual void process(const AudioBuffer& in, AudioBuffer& out,
                         std::uint32_t numFrames, const TimeInfo& time) noexcept = 0;
};
```
- **One method.** Input and output meet here — that's why I/O is one layer.
- `in` may be empty (output-only clock); `out` may be empty (input-only clock).
- **`noexcept`** and contractually **real-time-safe**: implementations must not
  allocate, lock, do I/O, or throw (ADR-0004). M1 defines the contract; the
  examples (§5) provide concrete implementations.

### 3.4 `AudioBackend` (`audio_backend.hpp`) — the swappable clock
The interface a transport implements to drive a `RenderCallback` (ADR-0005):

```cpp
class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual std::vector<AudioDeviceInfo> enumerate() = 0;            // setup-time
    virtual bool open(const StreamConfig&, RenderCallback*) = 0;     // setup-time
    virtual bool start() = 0;                                        // begin RT calls
    virtual void stop()  = 0;
    virtual std::uint32_t latencyFrames() const = 0;                 // for compensation
};
```
- `enumerate()`/`open()` are **setup-time** (may allocate); `start()`/`stop()`
  bound the real-time lifetime.
- **M1 ships the interface only** — the first concrete backend (Core Audio
  output) is **M2**.

### 3.5 `RingBuffer<T>` (`ring_buffer.hpp`) — the substantive code
A **single-producer / single-consumer (SPSC) wait-free** ring buffer — *the*
sanctioned way to move audio across a thread boundary (ADR-0004): process taps,
Python consumers, off-thread inference, recorders.

**Design decisions, line by line:**
- **`static_assert(is_trivially_copyable_v<T>)`** — only POD-like payloads
  (samples, frames), so copies are `memcpy`-cheap and RT-safe.
- **Power-of-two storage + bitmask index.** `minCapacity` is rounded up to a
  power of two; `mask_ = capacity_ - 1` turns wrap-around into a single `&`
  (no modulo, no branch). **One slot is reserved** to distinguish *full* from
  *empty*, so `capacity()` == rounded − 1 (e.g. `RingBuffer(10)` → storage 16,
  usable 15).
- **Allocation happens once**, in the constructor (`make_unique<T[]>`). `push`,
  `pop`, `write`, `read` are **wait-free and allocation-free**.
- **Memory ordering** (the correctness crux): the producer publishes its write
  index with `memory_order_release`; the consumer reads it with
  `memory_order_acquire` (and vice-versa). Each side reads *its own* index
  `relaxed`. This guarantees data written before the release is visible after the
  matching acquire — correct for exactly one producer + one consumer thread.
- **No overwrite policy.** `push`/`write` refuse when full (return `false` / a
  short count); `pop`/`read` return `false` / a short count when empty. The
  caller decides how to handle under/overflow (e.g. emit silence on underrun).
- **False-sharing avoidance.** The write and read cursors live in separate
  `alignas(hardware_destructive_interference_size, fallback 64)` structs, so the
  two threads don't ping-pong a shared cache line.

**API:**
```cpp
explicit RingBuffer(std::size_t minCapacity);
std::size_t capacity() const noexcept;
bool        push(const T& value) noexcept;          // producer, one element
bool        pop(T& out) noexcept;                   // consumer, one element
std::size_t write(const T* src, std::size_t count) noexcept;  // producer, bulk
std::size_t read(T* dst, std::size_t count) noexcept;         // consumer, bulk
std::size_t sizeApprox() const noexcept;            // may be stale
bool        empty() const noexcept;
```

### 3.6 Conversions (`conversions.hpp` / `conversions.cpp`)
The device⇄engine sample-format boundary. All four are `noexcept`,
allocation-free, and operate over caller-owned memory:

```cpp
void interleave  (const float* const* planar, float* interleaved,
                  std::uint32_t numChannels, std::uint32_t numFrames) noexcept;
void deinterleave(const float* interleaved,  float* const* planar,
                  std::uint32_t numChannels, std::uint32_t numFrames) noexcept;
void int16ToFloat(const std::int16_t* src, float* dst, std::size_t count) noexcept;  // ÷32768
void floatToInt16(const float* src, std::int16_t* dst, std::size_t count) noexcept;  // clamp[-1,1]·32767
```
- Planar layout: `planar[ch][frame]`; interleaved layout: `[frame*numChannels + ch]`.
- `int16ToFloat` scales by `1/32768` (so −32768 maps to −1.0 exactly).
- `floatToInt16` **clamps** to `[-1, 1]` before scaling by `32767`, preventing
  integer wrap on out-of-range input.
- This is the only compiled translation unit in the library.

---

## 4. Build system (`CMakeLists.txt`)

- **Project:** `aiudio` 0.0.1, **C++20**, extensions off, `RelWithDebInfo` default.
- **Target:** `add_library(aiudio_io src/io/conversions.cpp)` + the headers, with
  `include/` as a public include dir; alias **`aiudio::io`**. Built with
  `-Wall -Wextra -Wpedantic`.
- **Options:**
  - `AIUDIO_BUILD_TESTS` (ON) → `tests/`
  - `AIUDIO_BUILD_EXAMPLES` (ON) → `examples/cpp/`
  - `AIUDIO_SANITIZE` (string) → adds `-fsanitize=<value>` (`thread`, or
    `address,undefined`) compile/link flags.
- `CMAKE_EXPORT_COMPILE_COMMANDS` on (for clang-tidy/IDEs).

Build & test:
```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

---

## 5. Tests (`tests/`)

A deliberately tiny, **dependency-free** harness (`test_framework.hpp`:
`AIUDIO_TEST(name)`, `CHECK`/`REQUIRE`, `AIUDIO_TEST_MAIN()`) so the build stays
self-contained/offline; each test file is its own executable + CTest case. (A
richer framework like Catch2/GoogleTest can replace it later.)

- **`test_ring_buffer.cpp`** — capacity rounding; FIFO order; full-without-
  overwrite; bulk write/read round trip; and the **acceptance test**: a 1M-item
  single-producer/single-consumer **stress test** verifying no loss, in order.
- **`test_conversions.cpp`** — interleave/deinterleave round trip; int16⇄float
  round trip; float→int16 **clamping** of out-of-range values.

**Verified on macOS 26:** all pass, and the ring-buffer stress test is clean
under **ThreadSanitizer** (genuinely race-free) and **ASan/UBSan**.

---

## 6. Usage examples (`examples/cpp/`)

Teaching code (not part of the library) showing how to consume M1.
`example_support.hpp` provides reusable pieces: `SineSource` and `GainNode`
(concrete `RenderCallback`s), `OfflineDriver` (a device-free "manual pump" — the
ADR-0005 offline clock that allocates buffers once and calls `process()` per
block), and a minimal `WavWriter`.

| Example | Demonstrates |
|---|---|
| `ex_ring_buffer` | moving audio across a thread boundary with `RingBuffer` (+ integrity check) |
| `ex_render_callback` | implementing `RenderCallback` nodes and driving them with `OfflineDriver` |
| `ex_conversions` | the int16⇄float + interleave⇄planar device⇄engine round trip |
| `ex_offline_render_wav` | rendering a node offline to a playable WAV |

Each prints results and exits non-zero on failure, so they double as smoke tests.
All four build warning-free and run green.

---

## 7. How M1 maps to the architecture & ADRs

- **ADR-0003** (one IR, universal node contract) → `RenderCallback` is the node's
  audio-render entry point; `AudioBuffer` is the data it exchanges.
- **ADR-0004** (audio thread is sacred) → enforced by design: `RingBuffer`
  pre-allocates and is wait-free; `AudioBuffer`/conversions allocate nothing;
  RT methods are `noexcept`.
- **ADR-0005** (one duplex callback, swappable clock) → `RenderCallback` is the
  duplex callback; `AudioBackend` is the swappable clock; `OfflineDriver` (in
  examples) is the first stand-in clock.

M1 is the **floor**: it defines the shapes M2+ fill in and the graph engine plugs
into.

---

## 8. RT-safety summary

| Operation | RT-safe? | Notes |
|---|---|---|
| `AudioBuffer` construct/access | ✅ | pointers only |
| `RingBuffer` `push/pop/write/read` | ✅ | wait-free, no alloc |
| `RingBuffer` constructor | ⛔ (setup) | allocates storage once |
| conversions (all four) | ✅ | over caller memory, `noexcept` |
| `RenderCallback::process` | ✅ (by contract) | implementations must comply |
| `AudioBackend::enumerate/open` | ⛔ (setup) | may allocate |

---

## 9. What M1 deliberately leaves for later

- **No device I/O** — no Core Audio, no `AudioBackend` implementation. → **M2**
  (output backend = the engine heartbeat), **M3** (input).
- **No full-duplex clock**, latency measurement → **M4**.
- **No process taps / file / plugin-host backends** → **M5–M7**.
- **No Python bindings** → **M8**.
- **No graph engine** (nodes, scheduling) — that's a separate track that *uses*
  these contracts.

> **Since M1 (io components added later on the same contracts):** `WavReader`/`WavWriter`
> (M6, Python-bound), `Resampler` + `DriftCompensator` + `ResamplingSource` (M9.3/M9.5),
> `map_channels` (M9.2), and **`io::WavRecorder`** — a live WAV recorder built on the
> `RingBuffer` (§3.5): the audio thread pushes blocks, a writer thread drains them to a
> `WavWriter` off-thread (ADR-0004). See `docs/pipeline/71` and `docs/pipeline/80` for the current io surface.

## 10. Acceptance status (vs `71-*` M1)

| Criterion | Status |
|---|---|
| `brew install cmake`; library builds | ✅ |
| RingBuffer passes 2-thread SPSC stress test (no loss, wait-free) | ✅ (+ TSan/ASan clean) |
| interleave⇄planar + float conversion helpers tested | ✅ |

M1 is **complete** and under review in PR #1.

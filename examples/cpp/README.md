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

## Notes

- These run **without any audio device** — they use the `OfflineDriver` pump, not
  Core Audio. Live device playback arrives with the M2+ backends (`docs/71`).
- `out.wav` is git-ignored.

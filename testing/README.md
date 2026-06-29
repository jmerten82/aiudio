# aiudio — Testing Strategy (Phase 0)

> **Last updated:** 2026-06-29 · Scope: the graph spine + I/O layer + the Python
> bindings & control plane — i.e. **everything in Phase 0**. This branch
> (`feat/g7-control-plane`, PR #13) is the final addition before Phase 0 closes, so
> this document is also the **gate**: when every layer below is green, Phase 0's
> *correctness story* is complete.

This folder holds the **strategy** and the **cross-cutting / higher-level tests**.
Fine-grained unit tests stay next to the code they test (`tests/` for C++, `bindings/`
for the Python bindings); this document maps the whole picture.

---

## 1. Why a bespoke strategy (the risks that ordinary tests miss)

aiudio is a real-time audio framework, so beyond "does it compute the right numbers?"
it has four risks that a normal unit-test suite would *not* catch — and each maps to a
dedicated test layer:

| Risk (and the rule it would break) | How we test it |
|---|---|
| **The audio thread allocates / locks / blocks** (ADR-0004 — the sacred thread) | a global-`operator new` **allocation hook** asserts `process()` does **zero** heap allocations; **TSan** asserts no locks/races; **ASan/UBSan** assert no memory/UB faults |
| **The same graph behaves differently on different backends** (ADR-0009 invariant — *one IR, many backends*) | a **golden-file** offline render (bit-exact) + a **cross-backend** test asserting `numpy executor == offline WAV backend` |
| **The Python control plane stalls or races the audio thread** (ADR-0010) | a **flood-while-rendering** TSan race test in C++; **live device** tests assert the GIL is released and commands are accepted mid-stream |
| **Live device behavior is unobservable in CI** (no audio hardware on runners) | a **headless / liveness split**: liveness tests are gated behind `AIUDIO_LIVE=1` and self-skip everywhere else |

The rest is the standard pyramid (unit → integration → e2e), plus static analysis.

---

## 2. The headless / liveness split (the core constraint)

* **Headless** — runs anywhere, including CI and Linux. Deterministic. Covers *all* of
  the C++ engine and *all* of the Python surface **except** actually starting a device.
* **Liveness** — needs a real macOS output device. Gated by `AIUDIO_LIVE=1`; auto-skips
  otherwise (see `testing/python/conftest.py`). Live output is **silent today** (no
  signal-generator node yet), so these assert the *frontend contract* via telemetry +
  behavior — start/stop, render cadence, live-control acceptance, GIL release — not
  audio content.

---

## 3. C++ test layers

| Layer | What it proves | Where | Run |
|---|---|---|---|
| **Unit** | ring buffer, sample-format conversions, WAV I/O, graph build/validate, each node, the compiler/executor, the meter | `tests/test_{ring_buffer,conversions,wav_file,graph,graph_nodes,graph_executor,graph_meter,biquad_node}.cpp` | `ctest` |
| **Integration / golden** | a `file → graph → file` render is **bit-exact** against a golden (the one-IR contract, offline) | `tests/test_offline_render.cpp` | `ctest` |
| **Concurrency (TSan)** | live graph **edit** (atomic schedule swap) and the **control queue** are race-free under a flood while another thread renders | `tests/test_graph_live_edit.cpp`, `tests/test_graph_control.cpp` | `-DAIUDIO_SANITIZE=thread` |
| **Memory / UB (ASan/UBSan)** | no leaks, overflows, or undefined behaviour across the suite | the whole suite | `-DAIUDIO_SANITIZE=address,undefined` |
| **RT-safety (allocation)** ⭐ | `postParam` + `process()` (incl. biquad coeff recompute on the audio thread) perform **0 heap allocations** | `testing/cpp/test_rt_safety_alloc.cpp` | `ctest` |
| **Platform** | Core Audio device enumeration (macOS, no audio) | `tests/test_coreaudio_enumerate.cpp` | `ctest` (APPLE) |

The allocation test (`testing/cpp/`) is the one new piece worth calling out: it replaces
global `operator new`/`delete` with a guarded counter, runs 1000 blocks of
`postParam + process()` with the counter armed, and asserts it never ticked. A positive
self-check first proves the hook actually fires (a direct `::operator new` call, which —
unlike a `new` expression — the optimizer may not elide), so a pass can't be a false
negative. Under sanitizer builds the hook is compiled out (ASan/TSan supply their own
allocator) and the hot path is exercised for faults instead.

---

## 4. Python test layers

| Layer | What it proves | Where | Run |
|---|---|---|---|
| **Binding unit** | `source→gain→sink` returns numpy (`out == in·gain`); cycle rejected; live `set_gain`/`set_cutoff`; `render_count`; `DeviceBackend.enumerate` | `bindings/test_python_bindings.py` | `pytest` |
| **Cross-backend** ⭐ | **`numpy executor == offline WAV backend`** for the same graph (one IR, many backends); the control queue lands on the next block; meter telemetry | `testing/python/test_cross_backend.py` | `pytest` |
| **Packaging** | `pip install .` exposes the documented API (`__all__`, `__version__`, `WavFormat`, macOS `DeviceBackend`) — guards the "symbol built but not re-exported" regression | `testing/python/test_packaging.py` | `pytest` |
| **Notebooks** | both the teaching **tour** (`notebooks/`) and the **acceptance walkthrough** (`testing/notebooks/`, every feature + its shortcomings) execute end to end with **0 cell errors** (living docs can't rot) | `testing/python/test_notebook.py` (parametrized) | `pytest` (marked `slow`) |
| **Live device** ⭐ | start/stop lifecycle, **render cadence ≈ sr/block** (the C++ thread really ran the graph), live control accepted mid-stream, **GIL released** (Python loop runs concurrently), clean restart | `testing/python/test_live_device.py` | `AIUDIO_LIVE=1 pytest` (marked `live`) |
| **Static analysis** | lint (`ruff`); the bindings/library themselves are checked by `clang-format`/`clang-tidy` | `python/ bindings/ examples/ testing/` | `ruff check …` |

---

## 5. Layout

```
testing/
├── README.md                     ← this strategy
├── run.sh                        ← one entry point (see §6)
├── cpp/
│   ├── CMakeLists.txt            ← wired into ctest from the root CMakeLists
│   └── test_rt_safety_alloc.cpp  ← RT-safety: process() is allocation-free
├── notebooks/
│   ├── build_walkthrough.py      ← generator (source) for the walkthrough notebook
│   └── aiudio_acceptance_walkthrough.ipynb  ← every feature, step by step + shortcomings
└── python/
    ├── conftest.py               ← `aud` fixture + `live` marker gating
    ├── _graphs.py                ← shared graph/signal builders
    ├── test_cross_backend.py     ← one-IR-many-backends + control plane
    ├── test_live_device.py       ← gated live RT device frontend
    ├── test_packaging.py         ← public-API surface
    ├── test_notebook.py          ← execute every notebook (tour + walkthrough)
    └── requirements-dev.txt      ← pytest, ruff, nbclient, ipykernel, …
```

The **acceptance walkthrough** (`testing/notebooks/`) is the human-facing companion to
the automated suite: a step-by-step pass over *every* Python feature with an explicit
shortcoming callout after each, plus a consolidated shortcomings matrix. It's executed by
`test_notebook.py` (so it can't rot) and is regenerated from `build_walkthrough.py`.

Pytest config (markers `live`/`slow`, `testpaths`) lives in `pyproject.toml`
(`[tool.pytest.ini_options]`), so a bare `pytest` from the repo root discovers both
`testing/python` and `bindings`.

---

## 6. How to run

**Everything (headless):**
```bash
source .venv/bin/activate
testing/run.sh                 # C++ build+ctest, TSan, ASan/UBSan, pip install, ruff, pytest
testing/run.sh --no-sanitize   # faster: skip the sanitizer passes
testing/run.sh --live          # also run the live RT device layer (needs a macOS device)
```

**Granular:**
```bash
# C++
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
cmake -S . -B build-tsan -DAIUDIO_SANITIZE=thread           && ctest --test-dir build-tsan
cmake -S . -B build-asan -DAIUDIO_SANITIZE=address,undefined && ctest --test-dir build-asan

# Python
pip install . -r testing/python/requirements-dev.txt
ruff check python bindings examples testing
pytest testing/python bindings           # headless (live tests auto-skip)
AIUDIO_LIVE=1 pytest testing/python/test_live_device.py   # liveness (macOS + device)
pytest -m "not slow"                     # skip the notebook execution
```

---

## 7. CI (`.github/workflows/ci.yml`)

| Job | Runner | Covers |
|---|---|---|
| `cpp` | ubuntu + macos | build + full `ctest` (incl. RT-safety allocation test) |
| `sanitizers` | ubuntu | TSan (control + live edit) + ASan/UBSan (full suite) |
| `python` | macos (so `DeviceBackend` compiles) | `pip install .`, `ruff`, headless `pytest` incl. the notebook; live tests self-skip |
| `liveness` | self-hosted `[macos, audio]`, **manual** (`workflow_dispatch`) | `AIUDIO_LIVE=1` live device tests |

The liveness job needs real hardware, so it never runs on hosted runners — trigger it by
hand on a tagged self-hosted macOS box.

---

## 8. Coverage → invariant / ADR (traceability)

| Invariant / decision | Guarded by |
|---|---|
| **The audio thread is sacred** (CLAUDE §4.1, ADR-0004) | `test_rt_safety_alloc` (no alloc) · TSan `test_graph_control`/`test_graph_live_edit` (no locks/races) · ASan/UBSan |
| **Cross-thread only via SPSC / atomics** (§4.2, ADR-0004) | `test_ring_buffer` · `test_graph_control` (the command queue) · TSan |
| **One IR, many backends** (§4.3, ADR-0009) | `test_offline_render` (golden) · `test_cross_backend` (numpy == offline) |
| **Universal node contract** (§4.4) | `test_graph_nodes` · `test_biquad_node` · `test_graph_executor` |
| **One callback, swappable clock** (§4.5, ADR-0005) | `test_graph_executor` (executor *is* a RenderCallback) · offline + device backends |
| **Python never on the audio thread** (ADR-0002, ADR-0010) | `test_live_device` (GIL released, cadence) · `test_rt_safety_alloc` · the control queue tests |
| **`pip install .` yields a usable package** | `test_packaging` · the `python` CI job |
| **Docs don't rot** (living-docs protocol) | `test_notebook` |

---

## 9. Known gaps & deferred (honest)

- **No audio-content assertions on a live device** — live output is silent (no
  oscillator / file-source node, and the input/duplex/tap backends aren't bound to
  Python yet). Closing this needs a signal source; then a **loopback capture**
  (e.g. BlackHole) + FFT could assert real audio. Until then, liveness is telemetry-based.
- **Allocation hook vs RTSan** — the in-process `operator new` hook proves alloc-freedom
  on the *unit* path; running a **live device session under RTSan** (or a malloc logger)
  while Python floods commands would be the strongest end-to-end proof. Future hardening.
- **No property-based / fuzz testing** of graph construction yet (random DAGs through
  `validate` + compile). A good Phase-0.5 addition.
- **Neural-node / `realtime_capable` metadata** (invariants §4.6–§4.7) — no neural nodes
  exist yet, so nothing to test; arrives with Phase 1.
- **Plugin-host backend (I/O M7)** — Phase 3; out of scope here.

---

## 10. Definition of done — Phase 0 testing

- [x] C++ unit + integration + golden green (`ctest`).
- [x] Control plane & live edit **race-free** (TSan) and **memory-clean** (ASan/UBSan).
- [x] `process()` proven **allocation-free**, including the live control path.
- [x] **One IR, many backends** verified (numpy == offline; golden render).
- [x] Python public API + packaging verified; the tour notebook executes clean.
- [x] Live RT device frontend verified on hardware (cadence, control, GIL, restart).
- [x] One-command runner (`testing/run.sh`) + CI for the headless layers.

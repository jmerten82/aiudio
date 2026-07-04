# CLAUDE.md — Working Guidance for aiudio

Guidance for Claude Code (and humans) working in this repo. Loaded every session,
so it is kept **tight**; depth lives in [`docs/`](docs/). Keep this file and
[`README.md`](README.md) **current** — see [§9 Living-documents protocol](#9-living-documents-protocol).

> **Last updated:** 2026-07-04 · **Current phase:** Phase 2 — agent control plane + visual
> workbench (Phase 1 differentiable core **✅ COMPLETE, D0–D8**). **Phase 2: 4 of 5 releases shipped
> (K + A0–A2 + B0–B2 + C0–C2, PRs #50–#60).** `aiudio.workbench` (typed action space + log +
> graph↔JSON + capability manifest, ADR-0020/0021), `aiudio.server` (localhost FastAPI/WebSocket
> bridge, `aiudio[workbench]`, ADR-0019), `web/` (React + React Flow editor — see/edit by hand:
> add/connect/tune/delete, undo, drag-layout, save/load), `aiudio.agent` (grounded Claude tool-use
> over the action space, `aiudio[agent]`, ADR-0022), and `workbench.tune_to_target` (the graph
> tunes its own params to a target via the Phase-1 diff layer). **Remaining: R5** — agent
> self-extension (D0–D3, authoring new nodes; RT-safety gate ADR-0024). Plans:
> [`docs/pipeline/85`](docs/pipeline/85-phase2-agent-workbench-roadmap.md) (Phase 2) ·
> [`docs/pipeline/86`](docs/pipeline/86-r5-self-extension-plan.md) (R5 detail). Phase 0 (I/O layer +
> graph spine) is complete, and the **true multi-source I/O** track (`docs/pipeline/76`) was implemented
> off-schedule and merged. **Phase 1 (merged, #39–#48):** the optional `aiudio.diff` third
> executor (PyTorch, off-thread; ADR-0016/0017) runs the *same* `Graph` IR through autograd, with
> **C++↔torch parity** — auto-mirroring any graph (`param_value`/`node_config`/`sample_rate` +
> node-introspection enabler). The full DSP node library is differentiable (linear, trainable
> filters, dynamics/nonlinear/recursive via per-frame scans); plus a multi-res STFT loss + `fit`/
> checkpoint trainer, `match_target` (recover params from a target render), `export_to_graph`
> (write trained params back into the C++ RT graph — round-trip verified), a first **neural node**
> (torch `nn.Module` as a graph peer, trained jointly with DSP, `torch.export` deploy), and a DDSP
> `HarmonicSynth` timbre-match exemplar. Roadmap + status:
> [`docs/pipeline/79`](docs/pipeline/79-phase1-differentiable-core-roadmap.md). See [README Roadmap](README.md#roadmap).

---

## 1. What this project is (30-second orientation)

**aiudio** is an AI-native digital audio framework: classical DSP and neural
models as **first-class peers** in one signal graph, an **LLM agent** that
authors/edits the graph from natural language, built **ML-first** (differentiable,
trainable), for **both real-time and offline** use. Music production first; C++
real-time core + Python research/ML layer. Full vision:
[`docs/00-vision-and-scope.md`](docs/00-vision-and-scope.md).

## 2. Locked decisions — do not re-litigate

From `docs/00-vision-and-scope.md` §3:
- **AI role:** hybrid DSP+neural peers **and** agent control plane **and** ML-first
  workflow (all three).
- **Domain:** music production first; general audio research second.
- **Latency:** both real-time *and* offline, **configurable** — not a fork.
- **Languages:** **C++** real-time core + **Python** research/ML layer.

These — plus the core architecture choices — are formalized as **ADRs** (`adr/`,
0002–0024). If a change would violate one, stop and flag it — changing a locked
decision requires a **new superseding ADR** (§10), never a silent divergence.

## 3. Where things are

| Need | Go to |
|---|---|
| Vision, scope, non-goals | `docs/00-vision-and-scope.md` |
| Research dossier (SOTA) | `docs/theory/10-*` … `docs/theory/60-*` |
| Architecture & graph engine | `docs/theory/50-architecture-patterns.md` |
| macOS audio capture | `docs/pipeline/70-macos-audio-capture-plan.md` |
| **I/O foundation milestones** | `docs/pipeline/71-io-layer-milestones.md` |
| **Differentiable core (Phase 1)** | `docs/pipeline/79-phase1-differentiable-core-roadmap.md` · `python/aiudio/diff/` |
| References | `docs/90-references.md` |
| Roadmap & status | `README.md` §Roadmap |
| **Why** a decision was made | `adr/` (index: `adr/README.md`) |

## 4. Architecture invariants (the sacred rules)

These are non-negotiable; violating them is a bug even if it "works":

1. **The audio thread is sacred.** Inside any real-time callback / `process()` /
   device IOProc: **no heap allocation, no locks/mutexes, no syscalls/IO, no
   logging, no exceptions, no Python (GIL), no unbounded work.** Pre-allocate
   everything at `prepare()`/`open()`.
2. **Cross thread boundaries only via lock-free SPSC ring buffers / atomics.**
   (taps, Python consumers, off-thread inference, recorders.)
3. **One IR, many backends.** The typed graph IR is the single source of truth;
   real-time, offline, and differentiable executors all run *the same* IR. Don't
   fork representations.
4. **The node contract is universal.** Every node (DSP or neural) exposes
   `process()`, differentiable parameters, and metadata
   (`realtime_capable`, latency, causal, sample-rate). DSP and neural are peers.
5. **One duplex callback, swappable clock.** `process(in, out, frames, time)` is
   the one RT entry point; backends (device / plugin-host / offline) drive it.
6. **Runtime-agnostic neural nodes.** Abstract over LibTorch/ONNX/TFLite (ANIRA
   proves it); never bind the engine to a single ML runtime.
7. **`realtime_capable` and differentiability status are explicit, enforced
   node properties** — never assumed.

## 5. C++ best practices

- **C++20**, namespace `aiudio::`. RAII for all ownership; `std::unique_ptr` to
  own, raw pointers / `std::span` for non-owning views. No naked `new`/`delete`.
- **Real-time code is `noexcept`** and allocation-free. Exceptions are allowed in
  setup/teardown (non-RT) only; use error codes / `std::expected` across the RT
  boundary.
- **No `std::vector` resize, no `std::string`, no `std::shared_ptr` refcount
  churn** in the hot path. Pre-size buffers; reuse.
- Const-correctness throughout. Prefer free functions + small structs over deep
  hierarchies. Measure before adding SIMD (Eigen/XSIMD where it pays).
- **Format & lint:** `clang-format` + `clang-tidy`. **Sanitizers** (ASan/UBSan/
  TSan) in debug/CI. Consider a real-time-safety check (e.g. an allocation hook
  or RTSan) on `process()`.
- **Tests:** Catch2 or GoogleTest. Include a **golden-file** offline-render test
  (bit-exact) and an **xrun/latency** test for RT paths.

## 6. Python best practices

- **Python 3.11+** (target machine has 3.14). **Full type hints**; check with
  `pyright`/`mypy`. **Format & lint with `ruff`**.
- The Python layer is **research/control only** — it never touches the audio
  thread. Drive control-rate changes (agent edits, automation) via a lock-free
  queue into C++.
- **Bindings:** nanobind; expose audio as numpy (float32) blocks. **Packaging:**
  `pyproject.toml` + `scikit-build-core` (builds the CMake C++ core).
- **Tests:** `pytest`. Keep ML/training code reproducible (seed, pinned deps).

## 7. Don't reinvent

Lean on existing, proven pieces (see `docs/theory/10-*`, `docs/theory/30-*`): torchaudio /
librosa (analysis), **RTNeural** + **ANIRA** (RT inference), **JUCE** (plugin
hosting), libsndfile / AVFoundation (file I/O), **Neutone** / **nn~** (deploy &
interop), Core Audio (macOS I/O). Build the *unification*, not the parts.

## 8. Conventions

- **Provenance in docs:** mark claims **✓ Verified** (cited from a research pass)
  vs **○ Background** (confirm before relying). Keep this convention.
- **Commits:** Conventional Commits (`feat:`, `fix:`, `docs:`, `refactor:`,
  `test:`, `build:`). Small, focused commits. Only commit/push when the user asks;
  branch off `main` first if needed.
- **Naming:** C++ `PascalCase` types / `camelCase` methods / `snake_case` files;
  Python `snake_case`. Audio conventions: frames vs samples vs channels named
  explicitly; float32, deinterleaved (planar) internally.

## 9. Living-documents protocol

**README.md, CLAUDE.md, and the roadmap are living documents — keep them current
as the project moves.** This is an explicit standing instruction from the project
owner.

**Update triggers — in the *same* change that causes them:**
- **Completed/started a milestone** → tick the box in `README.md` §Roadmap and
  update the milestone status in `docs/pipeline/71-*` / `docs/theory/60-*`.
- **An architecturally-significant decision is made or changed** → write a new
  **ADR** (or supersede one) in `adr/`, and update the `adr/README.md` index +
  the `README.md` ADR table. See §10.
- **A scope, vision, or locked decision changes** → `README.md` +
  `docs/00-vision-and-scope.md` (and note why) **+ a superseding ADR**.
- **New architecture invariant, build step, or convention** → this file (§4–§8).
- **New tooling / dependency / tech-stack choice** → `README.md` §Tech stack +
  this file §5–§7.
- **New code layout** (`src/`, `python/`, …) → `README.md` §Repository structure.
- **New verified research** → the relevant `docs/*` + `docs/90-references.md`.

**Each time you update either file, bump its `Last updated:` date** (top of file)
to the current date. When in doubt, prefer updating docs over leaving them stale —
a wrong/stale doc is worse than none.

> If the owner wants this *enforced* (not just followed by convention), a
> session-end reminder hook can be wired via `settings.json`/`update-config` —
> offer it; don't assume it.

## 10. Architecture Decision Records (ADRs)

Load-bearing decisions are recorded in [`adr/`](adr/) — append-only, one decision
per file (full process in `adr/README.md`). `/docs` explains how the system works
today; **`/adr` records *why it got that way*.**

**Write an ADR when** a decision is hard to reverse or constrains future work:
core invariants (§4), language/runtime/threading boundaries, the public API / graph
IR, a major dependency or build-system choice, or platform/I/O strategy. **Skip
it** for easily-reversible, local, or stylistic choices — those live in code review
and §5–§8.

**How:** copy `adr/template.md` → `adr/NNNN-kebab-title.md` (next number); fill
Context → Decision → Consequences → Alternatives; set status; update the index in
`adr/README.md` and the ADR table in `README.md`. To change an Accepted ADR, write
a **new** one and mark the old `Superseded by ADR-XXXX` — never rewrite history.
**Accepted ADRs are binding; the invariants in §4 are their enforcement.**

## 11. How to work here

- **Build & test:** `cmake -S . -B build && cmake --build build -j && ctest
  --test-dir build --output-on-failure`. Sanitizers via
  `-DAIUDIO_SANITIZE=thread` or `=address,undefined`. Python M0 spikes: see
  `examples/README.md`. **Full strategy + one-shot runner (C++ & Python, headless
  vs. live): [`testing/README.md`](testing/README.md) / `testing/run.sh`.**
- **Doc-driven:** the design is written down before code. Read the relevant
  `docs/*` before implementing; reflect real changes back into the docs.
- **Vertical slices:** prefer the smallest end-to-end slice that proves a claim
  (e.g. capture→gain→playback) over broad horizontal scaffolding.
- **Verify on the real machine:** this is macOS 26 (Apple Silicon dev box).
  Capture/playback work needs TCC permissions and (for taps) a signed binary —
  see `docs/pipeline/70-*` §6.

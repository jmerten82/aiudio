# 86 — R5: Agent Self-Extension (authoring new nodes) — Implementation Plan

> **Last updated:** 2026-07-04 · **Scope:** the detailed plan for **Release R5 "it extends itself"**
> — milestones **D0–D3** of [`docs/pipeline/85`](85-phase2-agent-workbench-roadmap.md). When the
> built-in library can't express something, the agent **authors a new node type**; it lands in a
> **personal, local registry** (never shipping `main`), passes an **RT-safety gate**, and becomes a
> reusable node like any other. Governed by **ADR-0023** (personal registry & isolation) and
> **ADR-0024** (RT-safety gate & plugin ABI). · **Status:** 📋 planned. **Honest up front:** this is
> the heaviest, most environment-dependent milestone in the project — it compiles C++ at runtime and
> loads it into the engine. The pieces are individually testable; the full "agent invents novel C++
> and hot-loads it into live audio" is proof-of-concept, and the plan calls out where.

---

## Contents
- [1. What R5 delivers](#1-what-r5-delivers)
- [2. Where it sits](#2-where-it-sits)
- [3. The hard problems + feasibility](#3-the-hard-problems)
- [4. Architecture](#4-architecture)
- [5. Milestones D0–D3 (sub-PRs)](#5-milestones)
- [6. Dependency graph](#6-dependency-graph)
- [7. The RT-safety gate — detail](#7-the-rt-safety-gate)
- [8. The node-plugin ABI — detail](#8-the-node-plugin-abi)
- [9. Security model](#9-security-model)
- [10. Testing strategy](#10-testing-strategy)
- [11. Definition of done + risks](#11-definition-of-done)

---

## 1. What R5 delivers

The agent can **create a node type that doesn't exist yet** (a novel DSP block, or a wrapper), have
it **compile and load** into the running engine as a first-class node — usable in the graph, editable
in the UI, tunable by the diff layer — while the **audio-thread invariant is enforced on the
generated code** (ADR-0004/0024) and **nothing leaks into the shipping product** (ADR-0023).

Two properties do the heavy lifting (from earlier decisions):
- **Isolation is the primary safety property** — authored nodes live in a local, git-ignored
  registry, are **never auto-merged**, and are **disposable** (a bad one is deleted). The shipping
  product is never at risk regardless of what the agent generates.
- **Three gates, by concern** (don't conflate): an **automatic RT-safety pre-flight** (correctness,
  discard-on-fail, no ceremony); **inform + confirm** before an authored node touches the *live*
  audio thread (ADR-0022 §5.1a); **heavyweight review** only at promote-to-`main` (a PR).

## 2. Where it sits

Builds entirely on machinery that exists:
- **The node contract** (`node.hpp`): `prepare`/`process`/ports/`typeName`/`setParam`/`paramValue`/
  `config`/`paramDescriptors`/`latencyFrames`/`channelLayout`. A new node just implements it.
- **The action space + manifest** (A0/A1): a registered plugin kind is added via `add_node(kind)`
  and appears in the capability manifest exactly like a built-in — so the UI palette + the agent
  pick it up for free.
- **RCU recompile + atomic swap** (ADR-0010): inserting a newly-loaded node is a normal off-thread
  topology change; the audio thread never blocks.
- **The offline + differentiable executors**: the *quarantine* — a node that fails the RT gate can
  still run there (non-RT), so authoring isn't all-or-nothing.

The gap R5 fills: today `add_<kind>` factories are **hardcoded bindings**. R5 adds a **runtime node
registry** (kind → factory) that built-ins *and* dynamically-loaded plugins populate.

## 3. The hard problems + feasibility

| Problem | Approach | Feasibility here |
|---|---|---|
| **Compile C++ at runtime** | Each package ships source + a tiny CMake target; build to a shared lib off-thread. | ✅ clang++ 21 + cmake are present. ~seconds/build. |
| **Load into a running engine** | `dlopen` the lib **off the audio thread** → register the factory → `add_node` + RCU recompile. | ⚠️ ABI-stable C entry point needed; hot-load into *live* audio is the riskiest bit — **fallback:** register into a freshly-compiled schedule (the graph recompiles on any topology change anyway). |
| **RT-safety of generated code** | The D2 gate: static checks + a sanitizer/RTSan test render + contract tests → pass / quarantine / discard. | ⚠️ RTSan (`-fsanitize=realtime`) if the toolchain has it; **fallback:** an allocation-hook + a static source scan. |
| **Security (local code exec)** | Sandboxed build; **human confirm before RT-load**; no-network policy for generated nodes; provenance + one-click rollback; never auto-merged. | ✅ policy + confirm; single-user local. |
| **This environment** | Sample plugin, registry, scaffold, gate, and agent-authoring-via-mock are all testable headlessly. | ✅ unit-testable; the end-to-end *live-audio hot-load* is PoC. |

## 4. Architecture

```
agent (C1) detects a capability gap
   │  author_node(spec)  ── scaffold ──▶  package: manifest + src/*.cpp + tests + CMake
   ▼                                              │  build (off-thread, clang++/cmake)
RT-safety pre-flight (D2) ◀───────────────────────┘  → plugin .dylib/.so
   │  pass                       │ fail
   ▼                             ▼
local node registry           quarantine (non-RT executors) OR discard
   │  dlopen (off-thread) + register factory by kind
   ▼
NodeRegistry (kind → factory)  ── add_node(kind) [action space] ─▶ RCU recompile + atomic swap
   │  introspected into the capability manifest (A1)
   ▼
usable in the graph: UI palette · agent · diff-layer tuning   (promote → PR to main = optional)
```

**Key components**
- **Node-plugin ABI (§8)** — a stable `extern "C"` entry point a shared lib exports: ABI version +
  kind name + a factory (`Node*` / opaque) + metadata (ports, param descriptors, defaults, config).
- **NodeRegistry** — `kind → factory`, populated by built-ins *and* `dlopen`'d plugins; the backing
  store for a generalized `add_node(kind)` and for manifest introspection of plugin kinds.
- **Local registry (ADR-0023)** — a git-ignored dir (`~/.aiudio/nodes/<name>/`), each a package
  `{package.json, src/, tests/, build/<lib>}`; auto-discovered + loaded at startup; `promote → PR`.
- **Scaffold (`aiudio.nodegen`)** — spec → a Node-contract C++ file (from a template) + CMake + a
  golden/contract test.
- **RT-safety gate (§7)** — static + sanitizer/RTSan + contract, deciding RT-eligible / quarantine /
  discard.

## 5. Milestones

Each is a self-contained, tested sub-PR; the risky/agentic parts come last.

- **D0 — Package format + local registry + runtime NodeRegistry.** *No agent, no codegen yet.*
  The C ABI header; a `NodeRegistry` (C++) + bindings (`register_node_dir(path)`, generalize
  `add_node(kind)` to registry kinds); a **manually-authored sample plugin package** (e.g. a simple
  gain-like node) built by hand. **Tests:** load the sample plugin → it appears in the manifest →
  `add_node(kind)` works → it processes + parity-checks correctly.
- **D1 — Scaffold + build pipeline.** `aiudio.nodegen`: spec (name, params, a `process` body) →
  generate the package (C++ from template + CMake + a golden test) → **build** the plugin lib
  off-thread. **Tests:** scaffold a trivial node (a "hardgain" multiply), build, load, use it.
- **D2 — RT-safety pre-flight (§7).** Static checks + a sanitizer/RTSan test render + contract tests
  → pass / quarantine / discard. **Tests:** a clean node passes; a node that **allocates in
  `process()`** is caught → quarantined; a contract-violating node is rejected.
- **D3 — Agent authors end-to-end.** An `author_node(spec)` agent tool (NL → scaffold → build → gate
  → register), behind the **consent + provenance** guardrails; the agent then uses the new node.
  **Tests:** mock-client-driven authoring of a trivial node → it appears + is usable + rolls back;
  a live test gated on `ANTHROPIC_API_KEY`.

## 6. Dependency graph

```
D0 (ABI + registry + sample plugin) ─▶ D1 (scaffold + build) ─▶ D2 (RT-safety gate) ─▶ D3 (agent authors)
                                                                     │ needs the agent (C0/C1 ✅)
```

## 7. The RT-safety gate

The enforcement of ADR-0004 on generated code — an **automatic pre-flight** run before a node may
touch the audio thread. Three layers:
1. **Static analysis of `process()`** — reject the forbidden set: heap alloc (`new`/`malloc`),
   locks, syscalls/IO (`printf`/iostream/file), exceptions (`throw`), and unbounded/data-dependent
   loops. *Approach:* a clang AST check (clang-tidy custom matcher) where available, else a
   source-level lint + a **link-time symbol scan** of the built lib (no `malloc`/`pthread_mutex`
   references reachable from `process`).
2. **Runtime sanitizer render** — build the package's test with ASan/UBSan/TSan **+ a real-time
   check** (`-fsanitize=realtime` / RTSan if present, else an **allocation-hook** that aborts on any
   allocation inside `process()`), then render a golden block; any RT violation fails.
3. **Contract tests** — `process` is `noexcept`; ports match; `paramDescriptors` present; a
   golden-file render is deterministic.

**Outcome:** pass → RT-eligible; fail → **auto-quarantine to the non-RT (offline/diff) executors** or
discard — *no human ceremony for a failed build*. The **human confirm** (ADR-0022 §5.1a) is only for
the moment an RT-eligible authored node is loaded onto the **live** audio thread.

## 8. The node-plugin ABI

A minimal, versioned **C ABI** (C++ name-mangling would break across compilers/versions):

```c
// aiudio_node_plugin.h — the stable contract a plugin shared library exports.
#define AIUDIO_NODE_ABI 1
typedef struct { /* opaque handle to a heap Node */ } AiudioNode;
typedef struct {
    int          abi_version;          // == AIUDIO_NODE_ABI
    const char*  kind;                 // factory kind (add_node name)
    const char*  type_name;            // Node::typeName()
    void*      (*create)(void);        // → new NodeSubclass  (as aiudio::graph::Node*)
    void       (*destroy)(void*);      // delete
    const char*  descriptors_json;     // param descriptors + ports + defaults (for the manifest)
} AiudioNodePlugin;

const AiudioNodePlugin* aiudio_node_plugin(void);   // the single exported entry point
```

The engine `dlopen`s the lib, reads `aiudio_node_plugin()`, checks `abi_version`, and registers a
factory `kind → create/destroy` in the `NodeRegistry`. `create()` returns a `Node*` the engine owns
(RAII wrapper). Descriptors flow into the manifest so the plugin is UI/agent-visible. Versioning:
mismatched `abi_version` → refuse to load. Unload deferred (RCU makes live unload subtle — keep
loaded for the session).

## 9. Security model

Compiling + loading agent-generated code is **arbitrary local code execution** — treated seriously
even though it's single-user/local:
- **Sandboxed build** (restricted working dir; no network in the build); a **no-network / no-IO
  policy** for generated `process()` (also enforced by the RT gate's forbidden-syscall check).
- **Human confirm** before an authored node is loaded onto the **live** audio thread (ADR-0022).
- **Provenance** — every authored node records its spec + prompt + build log; **one-click rollback**
  (delete from the registry).
- **Never auto-merged** — the shipping product is only touched by an explicit, reviewed PR (ADR-0023).

## 10. Testing strategy

Everything below runs **headlessly** (no audio device, no API key):
- **D0**: a checked-in **sample plugin package** built in CI → loaded → manifest + `add_node` +
  parity render.
- **D1**: scaffold a trivial node in a temp dir → build → load → assert its output.
- **D2**: fixtures — a *clean* node (passes), an *allocating* node (quarantined), a *contract-
  breaking* node (rejected).
- **D3**: the agent authoring loop driven by a **mock LLM client** (as in C0) → asserts the node is
  scaffolded/built/registered/used; **live** authoring gated on `ANTHROPIC_API_KEY`.
- CI gets a small extra lane (a build toolchain is already present for the C++ jobs). The
  live-audio hot-load is exercised only on a real box (like the existing `AIUDIO_LIVE` device tests).

## 11. Definition of done + risks

**Done when:** the agent authors a new node from natural language; it passes the RT-safety pre-flight,
loads into the engine (or quarantines on fail), appears in the manifest/palette, is usable in the
graph + tunable, is reusable in a later session, and the audio-thread invariant holds throughout —
with provenance + rollback and no path into `main` except a reviewed PR.

**Risks / mitigations:**
- *Hot-load into live audio is hard* → fallback to recompile-a-fresh-schedule; ship the registry +
  gate first, live hot-load as a follow-up.
- *RTSan may be unavailable* → the allocation-hook + static/symbol scan is the floor.
- *Generated C++ quality/variety* → start with a constrained template (per-sample `process` bodies);
  broaden later. Quarantine + discard make failures cheap.
- *Scope* → D0→D3 are independently useful; D0 (registry + a hand-authored plugin) already proves the
  "plugins are first-class" architecture without any codegen.

---

### Cross-references
- **Parent roadmap:** [`docs/pipeline/85`](85-phase2-agent-workbench-roadmap.md) (R5 / D0–D3).
- **Why (ADRs):** [0023](../../adr/0023-personal-node-registry.md) (registry & isolation),
  [0024](../../adr/0024-rt-safety-gate-and-plugin-abi.md) (gate & ABI),
  [0004](../../adr/0004-realtime-safety-audio-thread.md) (sacred audio thread),
  [0010](../../adr/0010-python-control-plane.md) (RCU recompile), [0022](../../adr/0022-agent-runtime-and-consent-policy.md) (consent).
- **Builds on:** the node contract (`include/aiudio/graph/node.hpp`), `aiudio.workbench` (A0/A1),
  `aiudio.agent` (C0/C1).

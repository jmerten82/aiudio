# 84 — Differentiable & Trainable Graphs (cookbook)

> **Last updated:** 2026-07-01 · **Scope:** how to *train* an aiudio graph — make its parameters
> (and neural models) learnable, optimize them against a target, and deploy the result back to the
> real-time C++ core. Grounded in the merged `aiudio.diff` layer (Phase 1 · D0–D8, **✓ Verified**).
> This is the **fourth cookbook**: `docs/81` is *topology*, `docs/82` is *nodes*, `docs/83` is
> *live control*, and this one is **training** — the "ML-first" pillar (ADR-0016/0017/0018). The
> diff layer is **Python-only** and optional: `pip install "aiudio[diff]"` (PyTorch).

---

## Contents
- [0. The model — a third executor](#0-the-model)
- [1. Make a graph differentiable](#1-make-a-graph-differentiable)
- [2. How gradients are computed](#2-how-gradients-are-computed)
- [3. Losses](#3-losses)
- [4. Train — the `fit` harness](#4-train)
- [5. Match a target render](#5-match-a-target-render)
- [6. Round-trip to real time](#6-round-trip-to-real-time)
- [7. A neural node as a peer](#7-a-neural-node-as-a-peer)
- [8. A DDSP synth exemplar](#8-a-ddsp-synth-exemplar)
- [Appendix A — Differentiability status per node](#appendix-a--differentiability-status-per-node)
- [Appendix B — The pitch caveat & what's deferred](#appendix-b--the-pitch-caveat)
- [Appendix C — Cross-references](#appendix-c--cross-references)

---

## 0. The model

The differentiable layer is a **third executor** — alongside the real-time and offline C++ ones —
that interprets the **same `Graph` IR** through **PyTorch autograd**, off the audio thread
(ADR-0016). It exists so DSP parameters are trainable and neural models are first-class peers.

Two rules make it honest:

| Rule | Why |
|---|---|
| **The C++ real-time core is never touched** (ADR-0004) | the diff layer is research/ML only — heavy, batched, GPU/MPS-friendly. It reads the graph's *topology + params*; it does not run on the audio thread. |
| **Every diff node mirrors its C++ node numerically** (the *parity harness*) | each node has two implementations — the RT `process()` (alloc-free C++) and a torch `forward()` — kept identical in CI, so you can **train in torch and deploy in C++**. |

Autograd (reverse-mode AD) computes the gradients; there is no hand-written `backward()`. See §2.

---

## 1. Make a graph differentiable

Build a graph with the normal API, compile it (so parameters are initialized), then wrap it in a
`DiffExecutor`. It **auto-mirrors** the graph — reading topology (`nodes()`/`edges()`), per-node
params (`param_value`), config (`node_config`), and sample rate — so no manual setup is needed.

```python
import aiudio as a
import aiudio.diff as adiff
import torch

g = a.Graph()
s, gn, ws, k = g.add_source(), g.add_gain(0.8), g.add_waveshaper("tanh", 2.0, 0.5), g.add_sink()
g.connect(s, 0, gn, 0); g.connect(gn, 0, ws, 0); g.connect(ws, 0, k, 0)
a.GraphExecutor().compile(g, channels=1, sample_rate=48000.0, max_block=512)

de = adiff.DiffExecutor(g)                 # a torch nn.Module mirroring the graph
x = torch.randn(4, 1, 512)                 # [batch, channels, frames]
y = de(x)                                  # forward through gain → waveshaper
```

`DiffExecutor` is an `nn.Module`; the graph's parameters are its learnable `nn.Parameter`s. Override
a node's starting params with `init_params={node_id: {param_index: value}}`.

---

## 2. How gradients are computed

Every node's `forward()` is built from differentiable torch ops, so autograd records a tape and
`loss.backward()` walks it in reverse (the chain rule) — accumulating `dL/dparam` into each
`Parameter.grad`. One backward pass yields all gradients.

```python
loss = de(x).pow(2).mean()
loss.backward()
for name, p in de.named_parameters():
    print(name, p.grad)                    # e.g. _diff.<gain_id>.gain  tensor(...)
```

- **Recursive nodes** (DcBlocker, Compressor, Gate, Delay) run a per-frame scan; backprop through
  the loop *is* backpropagation-through-time (a block is the truncation window).
- **Non-smooth ops** use subgradients (`abs`, `max`, `where`, `clamp`); the hard-clip waveshaper
  uses a straight-through estimator; filters train in the **frequency domain** (see Appendix A).

Use `dtype=torch.float64` for `torch.autograd.gradcheck`; float32 for training.

---

## 3. Losses

`aiudio.diff` ships the audio workhorse — a **multi-resolution STFT loss** (spectral convergence +
log-magnitude L1 over several FFT sizes; the DDSP/Yamamoto objective) — plus `mse`/`l1`.

```python
stft = adiff.MultiResolutionSTFTLoss()     # captures spectral/timbral structure
d = stft(de(x), target)                    # a differentiable scalar
mse = adiff.mse(de(x), target)             # raw-sample MSE, when you want exact match
```

FFT sizes larger than the signal are clamped, so it's safe on short blocks.

---

## 4. Train

`fit` runs the standard loop — data → forward → loss → backward → step — over any `nn.Module`,
returning the loss history. `seed_everything` makes runs reproducible; checkpoints round-trip.

```python
target = x * 0.3
hist = adiff.fit(de, adiff.mse, lambda: (x, target), steps=300, lr=0.05, seed=0)
# hist[0] -> hist[-1] decreases; params converge

adiff.save_checkpoint(de, "ckpt.pt")
adiff.load_checkpoint(de, "ckpt.pt")       # restore trained params
```

`batch_fn()` returns an `(input, target)` pair each step — a fixed pair (as above) or fresh batches.

---

## 5. Match a target render

The headline slice: recover a graph's parameters from a target **render** by backprop.
`match_target` is `fit` specialized to a fixed target.

```python
# render a target with "secret" params, then recover them from a default init
target = adiff.DiffExecutor(g, init_params={gn: {0: 0.7}, ws: {0: 3.0, 1: 0.6}})(x).detach()
learner = adiff.DiffExecutor(g)
adiff.match_target(learner, x, target, loss_fn=adiff.MultiResolutionSTFTLoss(), steps=600, lr=0.03)
# learner's gain/drive/mix converge to 0.7 / 3.0 / 0.6
```

This is the "brighten the vocal / match the tone" workflow. On a multi-node graph the parameters
can entangle, so the acceptance is the **render match** (a perceptual objective — CLAP — is the
Phase-2 upgrade). See `examples/python/ex_diff_param_match.py`.

---

## 6. Round-trip to real time

Close the loop: write the trained parameters back into the compiled C++ graph, then run it in real
time. The C++ render matches the trained-torch render.

```python
n = adiff.export_to_graph(learner, ex)     # ex = the compiled GraphExecutor; writes via set_param
out = ex.process(x_np)                      # C++ render == trained-torch render
```

`set_param` is queued; run a `process()` block to drain it. Atomic params (gain/compressor/gate)
apply on the first block; smoothed params (mixer/pan/waveshaper/delay) settle over a few blocks.
Stateful nodes are compared **cold** (`assert_parity(..., warmup=0)`). Neural weights deploy by
model export, not `set_param` (§7).

---

## 7. A neural node as a peer

A neural model is a graph node like any other (ADR-0016 — DSP and neural are peers). Add a
`NeuralNode` and inject a torch `nn.Module` for it; it trains **jointly** with the DSP nodes.

```python
import torch.nn as nn
class TinyAmp(nn.Module):                                  # a learned per-sample nonlinearity
    def __init__(self, h=16):
        super().__init__(); self.net = nn.Sequential(nn.Linear(1, h), nn.Tanh(), nn.Linear(h, 1))
    def forward(self, x):
        b, c, n = x.shape; return self.net(x.reshape(-1, 1)).reshape(b, c, n)

g = a.Graph()
s, gn, nid, k = g.add_source(), g.add_gain(1.0), g.add_neural_node(), g.add_sink()
g.connect(s, 0, gn, 0); g.connect(gn, 0, nid, 0); g.connect(nid, 0, k, 0)
a.GraphExecutor().compile(g, channels=1, sample_rate=48000.0, max_block=512)

de = adiff.DiffExecutor(g, modules={nid: TinyAmp()})       # inject the module
adiff.fit(de, adiff.mse, lambda: (x, torch.tanh(2.5 * x)), steps=400, lr=0.02, seed=0)
exported = torch.export.export(de._diff[str(nid)]._module.eval(), (x,))   # deploy toward RT
```

The C++ `NeuralNode` is an **identity placeholder** in RT — real neural inference (RTNeural /
ANIRA / LibTorch) is **Phase 3** (ADR-0006). Training + export (`torch.export` → ONNX/ExecuTorch)
are what D7 delivers. See `examples/python/ex_diff_neural_node.py`.

---

## 8. A DDSP synth exemplar

`HarmonicSynth` is a differentiable **harmonic + filtered-noise** synth (the DDSP additive model)
at a fixed pitch, with learnable per-harmonic amplitudes + a noise gain. Train it to **match a
target timbre** with the STFT loss.

```python
adiff.seed_everything(0)
target = adiff.HarmonicSynth(f0=220.0, n_harmonics=32, noise=False)
with torch.no_grad():
    target.amps.copy_(torch.log(torch.expm1(torch.tensor([1.0 / (k + 1) for k in range(32)]))))
target_audio = target().detach()

synth = adiff.HarmonicSynth(f0=220.0, n_harmonics=32, noise=False)
adiff.fit(synth, adiff.MultiResolutionSTFTLoss(), lambda: (None, target_audio), steps=400, lr=0.05)
# synth.harmonic_amplitudes recovers the 1/n envelope; STFT distance collapses ~300x
```

The metric is the multi-res STFT distance; a perceptual **CLAP-embedding** distance is the Phase-2
hook. See `examples/python/ex_ddsp_synth_match.py`.

---

## Appendix A — Differentiability status per node

Each diff node declares a status (`de.differentiability_report()` → `full`/`surrogate`/`nondiff`):

| Node(s) | Status | Note |
|---|---|---|
| Gain, Mixer, Pan, Sum, Source/Sink | **full** | linear / plumbing |
| Waveshaper (tanh, softclip) | **full** | smooth nonlinearity |
| Waveshaper (hardclip) | **surrogate** | straight-through estimator through the clip |
| DcBlocker | **full** | recursive one-pole scan |
| Compressor, Gate, Delay | **surrogate** | level detector / hard threshold / feedback scan — a.e.-differentiable via subgradients + BPTT |
| Biquad (`DiffBiquad`) | **full** | trained via the **frequency-domain magnitude response** (direct-form IIR has poor time-domain gradients — `docs/20`); coeffs export to the C++ node (ADR-0018) |
| NeuralNode | **full** | the wrapped torch module |

---

## Appendix B — The pitch caveat

A multi-res STFT loss is **poor at pitch** (`docs/20` §2.1): an oscillator's frequency is not
learned by naive gradient descent (the loss surface is riddled with local minima at harmonic
spacings). Consequences:

- **`HarmonicSynth` fixes f0** and learns *timbre* — which the STFT loss is good at. Pitch-aware /
  staged training is a Phase-2+ concern.
- A couple of hard branches are honestly non-differentiable: the **gate's `threshold_db`** sits
  only in the hard `where` condition (a soft knee would sacrifice C++ parity), and the **delay
  time** is an integer. These are documented, not hidden.

**Deferred beyond Phase 1:** RT neural inference (Phase 3), spectral/convolution-reverb nodes,
codec / source-separation nodes (Phase 4), and the CLAP perceptual objective (Phase 2).

---

## Appendix C — Cross-references

- **Roadmap & status:** [`docs/79`](79-phase1-differentiable-core-roadmap.md) (Phase 1 · D0–D8).
- **Node library / tiers:** [`docs/78`](78-node-library-roadmap.md) (Tier 3 = differentiable + neural).
- **Why (ADRs):** [0016](../adr/0016-differentiable-execution-strategy.md) (differentiable executor),
  [0017](../adr/0017-autodiff-framework-pytorch.md) (PyTorch), [0018](../adr/0018-trainable-filter-form.md)
  (trainable-filter form), [0006](../adr/0006-runtime-agnostic-neural-inference.md) (RT neural inference).
- **Research grounding:** [`docs/20`](20-differentiable-dsp-and-neural-audio.md) (DDSP, filter gradients,
  the pitch caveat), [`docs/40`](40-ai-agents-for-audio.md) (agents; the CLAP perceptual-objective direction).
- **Sibling cookbooks:** [`docs/81`](81-pipeline-usage-patterns.md) · [`docs/82`](82-node-usage-patterns.md) · [`docs/83`](83-live-control-and-dynamic-graphs.md).
- **Examples:** `examples/python/ex_diff_param_match.py` · `ex_diff_neural_node.py` · `ex_ddsp_synth_match.py`.

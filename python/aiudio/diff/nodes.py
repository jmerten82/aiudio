"""Differentiable node implementations + the type registry (ADR-0016).

Each aiudio node type has a *dual face*: its C++ ``process()`` (real-time / offline) and — here —
a differentiable torch ``forward()`` (training). A registry maps ``type_name`` (as reported by
``Graph.nodes()`` / ``node_type()``) to a ``DiffNode`` subclass. Every node declares its
**differentiability status** (``Full`` / ``Surrogate`` / ``NonDiff``), enforcing the node contract
(CLAUDE.md §4.7).

D0 registers the trivial set (Source, Sink, Gain, Sum) that proves the executor spine; later
milestones (D1–D3) register the rest of the library, some as ``Surrogate`` (e.g. filters via the
SVF form — ``docs/20`` §2.2 / candidate ADR-0018) or with straight-through gradients (hard clip).
"""
from __future__ import annotations

import enum
import math

import torch
import torch.nn as nn


class Differentiability(enum.Enum):
    """How trainable a node is (invariant §4.7)."""

    FULL = "full"          # exact, well-behaved gradients
    SURROGATE = "surrogate"  # trainable via an approximation (e.g. SVF filter, straight-through)
    NONDIFF = "nondiff"    # frozen: passes through with no gradient to its parameters


_REGISTRY: dict[str, type["DiffNode"]] = {}


def register_diff_node(type_name: str):
    """Class decorator: register a `DiffNode` for the given aiudio node ``type_name``."""

    def _decorate(cls: type["DiffNode"]) -> type["DiffNode"]:
        _REGISTRY[type_name] = cls
        cls.type_name = type_name
        return cls

    return _decorate


def registered_types() -> list[str]:
    """Sorted list of node type names that have a differentiable implementation."""
    return sorted(_REGISTRY)


def make_diff_node(type_name: str, num_inputs: int, num_outputs: int,
                   init: dict[int, float] | None = None, *, param_reader=None,
                   config: dict | None = None, sample_rate: float = 48000.0) -> "DiffNode":
    """Instantiate the registered `DiffNode` for ``type_name`` (raises if unregistered).

    ``param_reader(index) -> float`` reads the live C++ node's param value (node-introspection
    enabler); ``config`` its non-numeric settings (e.g. waveshaper ``shape``); ``sample_rate`` the
    compiled rate. ``init`` overrides ``param_reader`` per index (explicit control)."""
    cls = _REGISTRY.get(type_name)
    if cls is None:
        raise NotImplementedError(
            f"no differentiable implementation for node type {type_name!r}; "
            f"registered: {registered_types()}"
        )
    return cls(num_inputs, num_outputs, dict(init or {}), param_reader=param_reader,
               config=dict(config or {}), sample_rate=float(sample_rate))


class DiffNode(nn.Module):
    """Base class for a node's differentiable face.

    ``forward(inputs)`` takes a list of ``[batch, channels, frames]`` tensors (one per input port,
    in port order) and returns one ``[batch, channels, frames]`` output. Learnable parameters are
    ``torch.nn.Parameter``s created in ``__init__``. Each param is resolved by :meth:`_param`:
    an explicit ``init`` override wins, else the live C++ value via ``param_reader`` (so a
    `DiffExecutor` auto-mirrors any graph), else a default. ``config`` / ``sample_rate`` carry the
    node's non-numeric settings and the compiled rate.
    """

    #: differentiability status (subclasses override)
    differentiability: Differentiability = Differentiability.FULL
    #: set by @register_diff_node
    type_name: str = ""

    def __init__(self, num_inputs: int, num_outputs: int, init: dict[int, float] | None = None, *,
                 param_reader=None, config: dict | None = None, sample_rate: float = 48000.0):
        super().__init__()
        self.num_inputs = int(num_inputs)
        self.num_outputs = int(num_outputs)
        self.init = dict(init or {})
        self.config = dict(config or {})
        self.sample_rate = float(sample_rate)
        self._param_reader = param_reader

    def _param(self, index: int, default: float = 0.0) -> float:
        """Resolve a param: explicit ``init`` override → live C++ value → ``default``."""
        if index in self.init:
            return float(self.init[index])
        if self._param_reader is not None:
            return float(self._param_reader(index))
        return float(default)

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:  # noqa: D401
        raise NotImplementedError


@register_diff_node("SourceNode")
class SourceDiffNode(DiffNode):
    """A graph input (0→1). Its output is the executor's external input tensor — the executor
    injects it, so ``forward`` is never called."""

    differentiability = Differentiability.FULL

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:  # pragma: no cover
        raise RuntimeError("SourceNode output is injected by the DiffExecutor, not computed")


@register_diff_node("SinkNode")
class SinkDiffNode(DiffNode):
    """A graph output (1→0 as a sink; identity here). The executor reads its input as the
    graph's output tensor."""

    differentiability = Differentiability.FULL

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:
        return inputs[0]


@register_diff_node("GainNode")
class GainDiffNode(DiffNode):
    """Scalar gain (1→1). Param index 0 = gain (``docs/82``). Learnable.

    Note: the C++ ``GainNode`` *smooths* gain changes (click-free, an RT detail); the diff face is
    a plain multiply — parity therefore holds in **steady state** (the parity harness warms up the
    C++ side so its smoother has settled)."""

    differentiability = Differentiability.FULL

    def __init__(self, num_inputs, num_outputs, init=None, **kw):
        super().__init__(num_inputs, num_outputs, init, **kw)
        self.gain = nn.Parameter(torch.tensor(self._param(0, 1.0), dtype=torch.float64))

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:
        return inputs[0] * self.gain.to(inputs[0].dtype)


@register_diff_node("SumNode")
class SumDiffNode(DiffNode):
    """Unity sum of N inputs (N→1). No parameters."""

    differentiability = Differentiability.FULL

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:
        out = inputs[0]
        for extra in inputs[1:]:
            out = out + extra
        return out


# ---- D1: stateless linear nodes -------------------------------------------------- #

@register_diff_node("MixerNode")
class MixerDiffNode(DiffNode):
    """Weighted sum of N inputs (N→1), each with its own **learnable** gain. Param index ``i`` =
    gain of input ``i`` (``docs/82``); width-preserving. Matches C++ ``MixerNode``:
    ``out = Σ_i in_i · gain_i``."""

    differentiability = Differentiability.FULL

    def __init__(self, num_inputs, num_outputs, init=None, **kw):
        super().__init__(num_inputs, num_outputs, init, **kw)
        gains = [self._param(i, 1.0) for i in range(self.num_inputs)]  # default 1.0 per input
        self.gains = nn.Parameter(torch.tensor(gains, dtype=torch.float64))

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:
        out = inputs[0] * self.gains[0].to(inputs[0].dtype)
        for i in range(1, len(inputs)):
            out = out + inputs[i] * self.gains[i].to(inputs[i].dtype)
        return out


@register_diff_node("PanNode")
class PanDiffNode(DiffNode):
    """Equal-power pan, mono → stereo (1→**2** ch, G8 width change). Param index 0 = pan (−1…+1),
    **learnable**. Matches C++ ``PanNode``: ``θ = (pan·0.5 + 0.5)·π/2``; ``L = x·cos θ``,
    ``R = x·sin θ`` (reads input channel 0)."""

    differentiability = Differentiability.FULL

    def __init__(self, num_inputs, num_outputs, init=None, **kw):
        super().__init__(num_inputs, num_outputs, init, **kw)
        self.pan = nn.Parameter(torch.tensor(self._param(0, 0.0), dtype=torch.float64))

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:
        x = inputs[0][:, :1, :]                       # mono source = channel 0
        theta = (self.pan.to(x.dtype) * 0.5 + 0.5) * (math.pi / 2.0)
        left = x * torch.cos(theta)
        right = x * torch.sin(theta)
        return torch.cat([left, right], dim=1)        # [batch, 2, frames]


# ---- D3: nonlinear + recursive nodes --------------------------------------------- #

@register_diff_node("WaveshaperNode")
class WaveshaperDiffNode(DiffNode):
    """Saturation (1→1): ``out = x·(1−mix) + shape(x·drive)·mix`` — matches C++ ``WaveshaperNode``.
    Learnable drive (index 0) + mix (index 1); the ``shape`` (tanh|softclip|hardclip) comes from
    ``config`` (node introspection). tanh/softclip are smooth (FULL); hardclip uses a
    **straight-through** gradient (SURROGATE) so training still flows."""

    def __init__(self, num_inputs, num_outputs, init=None, **kw):
        super().__init__(num_inputs, num_outputs, init, **kw)
        self.drive = nn.Parameter(torch.tensor(self._param(0, 1.0), dtype=torch.float64))
        self.mix = nn.Parameter(torch.tensor(self._param(1, 1.0), dtype=torch.float64))
        self._shape = int(round(self.config.get("shape", 0)))  # 0=tanh 1=softclip 2=hardclip
        self.differentiability = (
            Differentiability.SURROGATE if self._shape == 2 else Differentiability.FULL)

    def _shaped(self, u: torch.Tensor) -> torch.Tensor:
        if self._shape == 1:                              # softclip: x / (1 + |x|)
            return u / (1.0 + torch.abs(u))
        if self._shape == 2:                              # hardclip: clamp, straight-through grad
            return u + (torch.clamp(u, -1.0, 1.0) - u).detach()
        return torch.tanh(u)                              # tanh (default)

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:
        x = inputs[0]
        drive = self.drive.to(x.dtype)
        mix = self.mix.to(x.dtype)
        return x * (1.0 - mix) + self._shaped(x * drive) * mix


@register_diff_node("DcBlockerNode")
class DcBlockerDiffNode(DiffNode):
    """One-pole DC/rumble remover (1→1), **recursive**: ``y[n] = x[n] − x[n−1] + R·y[n−1]``,
    ``R = clamp(1 − 2π·corner/sr, 0, 0.99999)`` — matches C++ ``DcBlockerNode``. No learnable
    params (fixed filter); differentiable through its input via a per-frame scan (the recursion
    autodiff pattern for D3). ``corner_hz`` (config) and ``sample_rate`` come from introspection."""

    differentiability = Differentiability.FULL

    def _r(self) -> float:
        corner = float(self.config.get("corner_hz", 20.0))
        r = 1.0 - 2.0 * math.pi * corner / (self.sample_rate if self.sample_rate > 0 else 48000.0)
        return max(0.0, min(0.99999, r))

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:
        x = inputs[0]                                     # [batch, ch, frames]
        r = self._r()
        x1 = torch.zeros_like(x[..., 0])                  # per (batch, ch) state, init 0
        y1 = torch.zeros_like(x[..., 0])
        outs = []
        for f in range(x.shape[-1]):
            xf = x[..., f]
            y = xf - x1 + r * y1
            x1, y1 = xf, y
            outs.append(y)
        return torch.stack(outs, dim=-1)

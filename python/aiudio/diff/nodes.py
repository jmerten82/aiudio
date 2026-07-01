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
                   init: dict[int, float] | None = None) -> "DiffNode":
    """Instantiate the registered `DiffNode` for ``type_name`` (raises if unregistered)."""
    cls = _REGISTRY.get(type_name)
    if cls is None:
        raise NotImplementedError(
            f"no differentiable implementation for node type {type_name!r}; "
            f"registered: {registered_types()}"
        )
    return cls(num_inputs, num_outputs, dict(init or {}))


class DiffNode(nn.Module):
    """Base class for a node's differentiable face.

    ``forward(inputs)`` takes a list of ``[batch, channels, frames]`` tensors (one per input port,
    in port order) and returns one ``[batch, channels, frames]`` output tensor. Learnable
    parameters are ``torch.nn.Parameter``s created in ``__init__`` from ``init`` (a
    ``{param_index: value}`` map matching the C++ ``set_param`` indices in ``docs/82``).
    """

    #: differentiability status (subclasses override)
    differentiability: Differentiability = Differentiability.FULL
    #: set by @register_diff_node
    type_name: str = ""

    def __init__(self, num_inputs: int, num_outputs: int, init: dict[int, float]):
        super().__init__()
        self.num_inputs = int(num_inputs)
        self.num_outputs = int(num_outputs)
        self.init = init

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

    def __init__(self, num_inputs: int, num_outputs: int, init: dict[int, float]):
        super().__init__(num_inputs, num_outputs, init)
        gain0 = float(init.get(0, 1.0))  # index 0 = gain
        self.gain = nn.Parameter(torch.tensor(gain0, dtype=torch.float64))

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

    def __init__(self, num_inputs: int, num_outputs: int, init: dict[int, float]):
        super().__init__(num_inputs, num_outputs, init)
        gains = [float(init.get(i, 1.0)) for i in range(self.num_inputs)]  # default 1.0 per input
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

    def __init__(self, num_inputs: int, num_outputs: int, init: dict[int, float]):
        super().__init__(num_inputs, num_outputs, init)
        self.pan = nn.Parameter(torch.tensor(float(init.get(0, 0.0)), dtype=torch.float64))

    def forward(self, inputs: list[torch.Tensor]) -> torch.Tensor:
        x = inputs[0][:, :1, :]                       # mono source = channel 0
        theta = (self.pan.to(x.dtype) * 0.5 + 0.5) * (math.pi / 2.0)
        left = x * torch.cos(theta)
        right = x * torch.sin(theta)
        return torch.cat([left, right], dim=1)        # [batch, 2, frames]

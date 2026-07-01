"""The differentiable executor (ADR-0016).

`DiffExecutor` reads the **same** aiudio `Graph` (topology via ``nodes()``/``edges()``) and
evaluates it through a registry of differentiable nodes, with PyTorch autograd end-to-end. It is
an ``nn.Module``, so ``.parameters()`` yields every node's learnable params (feed them to an
optimizer) and ``.to(device/dtype)`` moves the whole graph.

Tensor convention: **``[batch, channels, frames]``** float tensors. D0 handles a single input
stream (all `SourceNode`s receive the same external tensor) and returns the first sink's output;
multi-stream source/sink binding (per-stream inputs/outputs) is a follow-up once stream indices
are exposed (D1+). The initial parameter values are supplied via ``init_params`` — reading them
back from the C++ nodes (getters) is a later refinement; topology already comes from the graph.
"""
from __future__ import annotations

import functools
from collections import deque

import torch
import torch.nn as nn

from .nodes import make_diff_node


class DiffExecutor(nn.Module):
    def __init__(self, graph, *, init_params: dict[int, dict[int, float]] | None = None,
                 dtype: torch.dtype = torch.float32, sample_rate: float = 48000.0):
        super().__init__()
        self._dtype = dtype
        self._sample_rate = float(sample_rate)
        init_params = init_params or {}

        # --- read the IR: topology (nodes/edges) + per-node params & config (introspection) ---
        self._meta: dict[int, tuple[str, int, int]] = {
            nid: (tname, nin, nout) for (nid, tname, nin, nout) in graph.nodes()
        }
        self._edges = [tuple(e) for e in graph.edges()]  # (src, src_port, dst, dst_port)

        # --- build a diff node per graph node, auto-reading its live params + config ---
        self._diff = nn.ModuleDict()
        self._sources: list[int] = []
        self._sinks: list[int] = []
        for nid, (tname, nin, nout) in self._meta.items():
            reader = functools.partial(graph.param_value, nid)  # reader(index) -> live C++ value
            config = dict(graph.node_config(nid))
            self._diff[str(nid)] = make_diff_node(
                tname, nin, nout, init_params.get(nid, {}),
                param_reader=reader, config=config, sample_rate=self._sample_rate)
            if tname == "SourceNode":
                self._sources.append(nid)
            elif tname == "SinkNode":
                self._sinks.append(nid)
        if not self._sinks:
            raise ValueError("graph has no SinkNode — nothing to output")

        # --- adjacency: (dst_node) -> {dst_port: (src_node, src_port)} ---
        self._in_edges: dict[int, dict[int, tuple[int, int]]] = {}
        for (src, sp, dst, dp) in self._edges:
            self._in_edges.setdefault(dst, {})[dp] = (src, sp)

        self._order = self._topological_order()
        self.to(dtype)

    # ------------------------------------------------------------------ #
    def _topological_order(self) -> list[int]:
        ids = list(self._meta)
        indeg = {nid: 0 for nid in ids}
        adj: dict[int, list[int]] = {nid: [] for nid in ids}
        for (src, _sp, dst, _dp) in self._edges:
            adj[src].append(dst)
            indeg[dst] += 1
        # deterministic order: process ready nodes in ascending id
        ready = deque(sorted(n for n in ids if indeg[n] == 0))
        order: list[int] = []
        while ready:
            n = ready.popleft()
            order.append(n)
            for m in sorted(adj[n]):
                indeg[m] -= 1
                if indeg[m] == 0:
                    ready.append(m)
        if len(order) != len(ids):
            raise ValueError("graph is not a DAG (cycle detected) — cannot differentiate")
        return order

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """Run one batched block. ``x``: ``[batch, channels, frames]`` (the single input stream,
        fed to every SourceNode in D0). Returns the first sink's ``[batch, channels, frames]``."""
        x = x.to(self._dtype)
        outputs: dict[int, torch.Tensor] = {}
        for nid in self._order:
            tname, nin, _nout = self._meta[nid]
            if tname == "SourceNode":
                outputs[nid] = x
                continue
            in_map = self._in_edges.get(nid, {})
            ins: list[torch.Tensor] = []
            for port in range(nin):
                edge = in_map.get(port)
                if edge is None:
                    raise ValueError(f"node {nid} ({tname}) input port {port} is unconnected")
                src, _sp = edge
                ins.append(outputs[src])
            outputs[nid] = self._diff[str(nid)](ins)
        return outputs[self._sinks[0]]

    # ------------------------------------------------------------------ #
    @property
    def source_ids(self) -> list[int]:
        return list(self._sources)

    @property
    def sink_ids(self) -> list[int]:
        return list(self._sinks)

    def differentiability_report(self) -> dict[int, str]:
        """node id → differentiability status ('full' | 'surrogate' | 'nondiff')."""
        return {nid: self._diff[str(nid)].differentiability.value for nid in self._meta}

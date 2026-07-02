"""D7 — the first neural node (Phase 1).

A `NeuralNode` (a torch nn.Module) is a first-class graph peer: it composes with DSP nodes and
trains jointly in the differentiable executor. The C++ side is an identity placeholder (RT neural
inference is Phase 3 / ADR-0006); deployment is by exporting the trained module (TorchScript).
Gated on PyTorch.
"""
from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")
adiff = pytest.importorskip("aiudio.diff")
import torch.nn as nn  # noqa: E402

import aiudio as a  # noqa: E402

SR = 48000.0
BLOCK = 512


class TinyNet(nn.Module):
    """A tiny learned per-sample nonlinearity (NAM-flavored): MLP applied to each sample."""

    def __init__(self, hidden=8):
        super().__init__()
        self.net = nn.Sequential(nn.Linear(1, hidden), nn.Tanh(), nn.Linear(hidden, 1))

    def forward(self, x):                                   # [batch, ch, frames]
        b, c, n = x.shape
        return self.net(x.reshape(-1, 1)).reshape(b, c, n)


def _neural_graph(with_gain=False):
    g = a.Graph()
    s = g.add_source()
    prev = s
    gid = None
    if with_gain:
        gid = g.add_gain(0.8)
        g.connect(prev, 0, gid, 0)
        prev = gid
    nid = g.add_neural_node()
    k = g.add_sink()
    g.connect(prev, 0, nid, 0)
    g.connect(nid, 0, k, 0)
    a.GraphExecutor().compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    return g, nid, gid


def test_neural_node_in_graph_and_cpp_passthrough():
    g, nid, _ = _neural_graph()
    assert dict(g.node_config(nid)) == {"neural": 1.0, "realtime_capable": 0.0}
    # the C++ NeuralNode is an identity placeholder (RT inference is Phase 3)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    x = np.full((1, BLOCK), 0.4, np.float32)
    assert np.allclose(ex.process(x), 0.4, atol=1e-6)


def test_neural_node_requires_a_module():
    g, _nid, _ = _neural_graph()
    with pytest.raises(ValueError):
        adiff.DiffExecutor(g)                               # no module injected for the neural node


def test_neural_node_trains_as_a_peer():
    adiff.seed_everything(0)                                # seed BEFORE the net init (reproducible)
    g, nid, _ = _neural_graph()
    de = adiff.DiffExecutor(g, modules={nid: TinyNet()})
    # net weights are part of the executor's parameters
    assert any(f"_diff.{nid}." in name for name, _ in de.named_parameters())
    x = torch.randn(4, 1, BLOCK)
    target = torch.tanh(3.0 * x) + 0.2 * x                  # a nonlinearity to learn
    hist = adiff.match_target(de, x, target, steps=500, lr=0.03)
    assert hist[-1] < hist[0] * 1e-2                         # the net learned the shape
    assert adiff.max_abs_diff(de(x), target) < 0.05         # incl. the tails


def test_joint_dsp_and_neural_training():
    g, nid, gid = _neural_graph(with_gain=True)             # source → gain → neural → sink
    de = adiff.DiffExecutor(g, modules={nid: TinyNet()}, dtype=torch.float64)
    x = torch.randn(2, 1, BLOCK, dtype=torch.float64)
    target = torch.tanh(2.0 * x)
    de(x).sub(target).pow(2).mean().backward()
    params = dict(de.named_parameters())
    # BOTH the DSP gain and the neural weights receive gradients (jointly trainable peers)
    assert params[f"_diff.{gid}.gain"].grad is not None
    assert torch.isfinite(params[f"_diff.{gid}.gain"].grad).all()
    assert any(f"_diff.{nid}." in name and p.grad is not None and torch.isfinite(p.grad).all()
               for name, p in de.named_parameters())


def test_trained_module_exports_for_deployment():
    adiff.seed_everything(0)
    g, nid, _ = _neural_graph()
    model = TinyNet()
    de = adiff.DiffExecutor(g, modules={nid: model})
    x = torch.randn(1, 1, 64)
    adiff.match_target(de, x, torch.tanh(2.0 * x), steps=50, lr=0.02)
    # deploy path (ADR-0006, Phase 3): the trained module exports as a portable graph via
    # torch.export (→ ONNX / ExecuTorch / LibTorch); the modern, non-deprecated replacement for
    # TorchScript. Verify the exported program reproduces the eager output.
    exported = torch.export.export(model.eval(), (x,))
    assert torch.allclose(exported.module()(x), model(x), atol=1e-6)

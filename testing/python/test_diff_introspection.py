"""Node-introspection enabler + DiffExecutor auto-build (Phase 1, ADR-0016).

The C++ nodes now expose their current param values (`Graph.param_value`), non-numeric config
(`Graph.node_config`), and the compiled sample rate (`GraphExecutor.sample_rate`), so a
`DiffExecutor` mirrors *any* graph faithfully with **no `init_params`** — the graph is the single
source of truth. Verifies the getters and the auto-build parity.

Gated on PyTorch (auto-build tests); the pure-introspection getters run without torch too, but
kept here for cohesion.
"""
from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")
adiff = pytest.importorskip("aiudio.diff")

import aiudio as a  # noqa: E402

SR = 48000.0
BLOCK = 256


def _compiled(build, channels=1):
    """build(g) -> node_id; wraps it source→node→sink, compiles (so prepare() runs), returns
    (graph, node_id). Mixer/Pan use SmoothedValue, initialized at prepare()."""
    g = a.Graph()
    src, snk = g.add_source(), g.add_sink()
    nid = build(g)
    g.connect(src, 0, nid, 0)
    g.connect(nid, 0, snk, 0)
    a.GraphExecutor().compile(g, channels=channels, sample_rate=SR, max_block=BLOCK)
    return g, nid


def test_param_value_reads_current_values():
    g, gn = _compiled(lambda g: g.add_gain(0.42))
    assert abs(g.param_value(gn, 0) - 0.42) < 1e-6
    g, pn = _compiled(lambda g: g.add_pan(-0.3), channels=2)
    assert abs(g.param_value(pn, 0) - (-0.3)) < 1e-6
    # mixer: 2 inputs both fed by the source (fan-out)
    g = a.Graph()
    src, mx, snk = g.add_source(), g.add_mixer(2, 1.0), g.add_sink()
    g.connect(src, 0, mx, 0)
    g.connect(src, 0, mx, 1)
    g.connect(mx, 0, snk, 0)
    a.GraphExecutor().compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    assert abs(g.param_value(mx, 0) - 1.0) < 1e-6 and abs(g.param_value(mx, 1) - 1.0) < 1e-6


def test_node_config_exposes_construction_settings():
    g = a.Graph()
    ws = g.add_waveshaper("softclip", 2.0, 0.5)
    bq = g.add_biquad_highpass(80.0, 0.707, SR)
    dc = g.add_dc_blocker(25.0)
    assert dict(g.node_config(ws)) == {"shape": 1.0}          # 0=tanh 1=softclip 2=hardclip
    assert dict(g.node_config(bq)) == {"type": 1.0}           # 0=LP 1=HP 2=peak 3=LS 4=HS
    assert abs(dict(g.node_config(dc))["corner_hz"] - 25.0) < 1e-6


def test_executor_sample_rate():
    g = a.Graph()
    s, gn, k = g.add_source(), g.add_gain(1.0), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = a.GraphExecutor()
    assert ex.sample_rate == 0.0                              # not compiled yet
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    assert ex.sample_rate == SR


def test_diffexecutor_autobuilds_without_init_params():
    g = a.Graph()
    s, gn, k = g.add_source(), g.add_gain(0.5), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)

    de = adiff.DiffExecutor(g)                                # NO init_params — reads 0.5 from C++
    x = np.full((1, BLOCK), 0.8, np.float32)
    assert adiff.assert_parity(ex, de, x, tol=1e-5) <= 1e-5
    out = de(torch.from_numpy(x).unsqueeze(0)).squeeze(0)
    assert np.allclose(out.detach().numpy(), 0.4, atol=1e-6)  # 0.8 * 0.5, gain auto-read


def test_autobuild_reflects_setparam():
    # a mixer whose per-input gains are set via set_param; the DiffExecutor reads the set targets.
    g = a.Graph()
    src = g.add_source()
    mx = g.add_mixer(2, 1.0)
    snk = g.add_sink()
    g.connect(src, 0, mx, 0)
    g.connect(src, 0, mx, 1)
    g.connect(mx, 0, snk, 0)
    ex = a.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    ex.set_param(mx, 0, 0.5)
    ex.set_param(mx, 1, 0.3)
    # set_param is queued and applied on the audio thread — run a block to drain it so the graph
    # node's targets reflect 0.5/0.3 before the DiffExecutor auto-reads them.
    x = np.full((1, BLOCK), 1.0, np.float32)
    ex.process(x)

    de = adiff.DiffExecutor(g)                                # auto-reads the set gains (0.5, 0.3)
    assert adiff.assert_parity(ex, de, x, tol=1e-4, warmup=64) <= 1e-4
    out = de(torch.from_numpy(x).unsqueeze(0)).squeeze(0)
    assert np.allclose(out.detach().numpy(), 0.8, atol=1e-3)  # 0.5 + 0.3


def test_init_params_override_still_wins():
    g = a.Graph()
    s, gn, k = g.add_source(), g.add_gain(0.5), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = a.GraphExecutor()
    ex.compile(g, channels=1, sample_rate=SR, max_block=BLOCK)
    de = adiff.DiffExecutor(g, init_params={gn: {0: 0.9}})    # override the C++ 0.5
    out = de(torch.ones(1, 1, BLOCK)).squeeze(0)
    assert np.allclose(out.detach().numpy(), 0.9, atol=1e-6)

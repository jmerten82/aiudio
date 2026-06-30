"""Python binding-gap closures: biquad highpass/coeffs, graph introspection + edits,
XrunPolicy, and device-backend disconnect/xrun telemetry.
"""
from __future__ import annotations

import numpy as np
import pytest

SR = 48000.0


def test_biquad_highpass_blocks_dc(aud):
    g = aud.Graph()
    s, hp, k = g.add_source(), g.add_biquad_highpass(1000.0, 0.707, SR), g.add_sink()
    g.connect(s, 0, hp, 0)
    g.connect(hp, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=4096)
    out = ex.process(np.ones((1, 4096), np.float32))
    assert abs(float(out[0, -1])) < 1e-3            # high-pass blocks DC

    # set_cutoff/set_q drive it live (it re-designs as high-pass).
    assert ex.set_cutoff(hp, 200.0)
    assert ex.set_q(hp, 1.0)


def test_biquad_raw_coeffs_identity(aud):
    g = aud.Graph()
    s, bq, k = g.add_source(), g.add_biquad_coeffs(1.0, 0.0, 0.0, 0.0, 0.0), g.add_sink()
    g.connect(s, 0, bq, 0)
    g.connect(bq, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)
    out = ex.process(np.full((1, 64), 0.3, np.float32))
    assert np.allclose(out[0], 0.3, atol=1e-6)      # y[n] = x[n]


def test_graph_introspection(aud):
    g = aud.Graph()
    s, gn, k = g.add_source(), g.add_gain(0.5), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    assert g.nodes() == [(s, "SourceNode", 0, 1), (gn, "GainNode", 1, 1), (k, "SinkNode", 1, 0)]
    assert g.edges() == [(s, 0, gn, 0), (gn, 0, k, 0)]
    assert g.node_type(gn) == "GainNode"
    assert g.node_type(999) is None
    assert g.live_node_count == 3


def test_disconnect_and_remove_keep_ids_stable(aud):
    g = aud.Graph()
    s, gn, k = g.add_source(), g.add_gain(0.5), g.add_sink()
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)

    assert g.disconnect(gn, 0, k, 0) is True
    assert g.disconnect(gn, 0, k, 0) is False        # already gone
    assert g.edges() == [(s, 0, gn, 0)]

    assert g.remove_node(gn) is True
    assert g.node_type(gn) is None                   # tombstoned
    assert g.node_type(s) == "SourceNode"            # other ids unchanged
    assert g.node_count == 3 and g.live_node_count == 2
    assert g.edges() == []                           # edge touching gn dropped
    assert g.remove_node(gn) is False                # already removed


def test_edit_then_recompile_runs(aud):
    g = aud.Graph()
    s, atten, boost, k = g.add_source(), g.add_gain(0.5), g.add_gain(2.0), g.add_sink()
    g.connect(s, 0, atten, 0)
    g.connect(atten, 0, boost, 0)
    g.connect(boost, 0, k, 0)
    g.remove_node(atten)            # drop the attenuator
    assert g.connect(s, 0, boost, 0)  # rewire around the hole
    ok, err = g.validate()
    assert ok, err
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=16)
    out = ex.process(np.ones((1, 16), np.float32))
    assert np.allclose(out[0], 2.0)   # only boost(2.0) survives


def test_connect_rejects_removed_node(aud):
    g = aud.Graph()
    s, gn = g.add_source(), g.add_gain(0.5)
    g.remove_node(gn)
    assert g.connect(s, 0, gn, 0) is False
    assert g.edges() == []


def test_xrun_policy_enum(aud):
    assert aud.XrunPolicy.BestEffort != aud.XrunPolicy.Strict
    # exposed values
    assert {aud.XrunPolicy.BestEffort, aud.XrunPolicy.Strict}


@pytest.mark.skipif(
    not hasattr(__import__("aiudio"), "DeviceBackend"), reason="Core Audio backends are macOS-only"
)
def test_device_backends_expose_disconnect_xrun_telemetry(aud):
    # The real backends now surface the hot-plug/xrun model (M9.4) + accept an xrun_policy.
    for name in ("DeviceBackend", "InputBackend", "DuplexBackend", "TapBackend"):
        cls = getattr(aud, name)
        for attr in ("disconnected", "xrun_count", "set_disconnect_handler"):
            assert hasattr(cls, attr), f"{name}.{attr} missing"

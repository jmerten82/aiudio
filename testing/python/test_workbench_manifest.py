"""A1 — the capability manifest (Phase 2, ADR-0021).

The manifest is introspected from the real node registry (param descriptors from the C++
`paramDescriptors`), grounds the UI + agent, and drives set_param validation. Pure aiudio core.
"""
from __future__ import annotations

import pytest

import aiudio as a
from aiudio import workbench as wb


def test_every_descriptor_is_self_consistent():
    # for ALL described nodes: named, indexed, and default within its own suggested range
    for kind, entry in wb.capability_manifest()["kinds"].items():
        for d in entry["params"]:
            assert d["name"], f"{kind}: unnamed descriptor {d}"
            assert d["index"] >= 0
            assert d["min"] <= d["default"] <= d["max"], (
                f"{kind}.{d['name']}: default {d['default']} outside [{d['min']}, {d['max']}]")


def test_descriptor_defaults_match_real_node_defaults():
    # the descriptor `default` must equal the node's actual value after compile (smoothed params
    # settle). Covers every node with a canonical construction default AND a paramValue impl.
    # Excluded: biquads (no arg-free default — q/freq/gain_db are factory-set UI suggestions);
    # stereo_width/oscillator/noise (paramValue not yet implemented — introspection-enabler follow-up).
    man = wb.capability_manifest()["kinds"]
    for kind in ("gain", "pan", "waveshaper", "delay", "compressor", "gate", "mixer"):
        g = a.Graph()
        args = {"gain": 1.0} if kind == "gain" else {}
        nid = getattr(g, "add_" + kind)(**args)
        assert a.GraphExecutor().compile(g, channels=2, sample_rate=48000.0, max_block=128)
        for d in man[kind]["params"]:
            got = g.param_value(nid, d["index"])
            assert abs(got - d["default"]) < 1e-3, (
                f"{kind}.{d['name']}: descriptor default {d['default']} != node value {got}")


def test_manifest_covers_the_palette():
    m = wb.capability_manifest()["kinds"]
    # every factory kind has an entry
    assert set(m) == set(wb.available_kinds())
    for kind in ("gain", "compressor", "waveshaper", "delay", "gate", "pan"):
        assert kind in m and m[kind]["type"].endswith("Node")


def test_compressor_param_descriptors():
    comp = wb.capability_manifest()["kinds"]["compressor"]["params"]
    names = [d["name"] for d in comp]
    assert names == ["threshold_db", "ratio", "attack_ms", "release_ms", "makeup_db"]
    thr = comp[0]
    assert thr["index"] == 0 and thr["unit"] == "dB"
    assert thr["min"] == -60.0 and thr["max"] == 0.0 and thr["default"] == -18.0


def test_ports_and_rt_capability():
    m = wb.capability_manifest()["kinds"]
    assert m["gain"]["num_inputs"] == 1 and m["gain"]["num_outputs"] == 1
    assert m["oscillator"]["num_inputs"] == 0                 # generator: 0 in → 1 out
    # the neural node is a placeholder in RT (ADR-0006 / 0022)
    assert m["neural_node"]["realtime_capable"] is False
    assert m["gain"]["realtime_capable"] is True


def test_mixer_params_scale_with_inputs():
    # variable-arity: a described-per-input node
    g_params = wb.node_manifest("mixer")["params"]           # default 2 inputs
    assert [d["name"] for d in g_params] == ["gain[0]", "gain[1]"]


def test_manifest_includes_construction_defaults():
    m = wb.capability_manifest()["kinds"]
    assert m["gain"]["defaults"] == {"gain": 1.0}          # a factory that requires args
    assert "freq" in m["biquad_peaking"]["defaults"]
    assert m["compressor"]["defaults"] == {}               # all-default factory


def test_add_any_kind_via_manifest_defaults():
    # the "add node" path (B1): manifest defaults let you construct even required-arg factories
    m = wb.capability_manifest()["kinds"]
    s = wb.GraphSession()
    for kind in ("gain", "biquad_peaking", "channel_matrix", "compressor", "source"):
        s.add_node(kind, dict(m[kind]["defaults"]))
    assert len(s.to_document()["nodes"]) == 5


def test_param_issues():
    assert wb.param_issues("compressor", 1, 4.0) == []       # ratio in range
    assert wb.param_issues("compressor", 99, 0.0)            # unknown index
    assert wb.param_issues("compressor", 0, 50.0)            # threshold_db way out of range
    assert wb.param_issues("nonesuch", 0, 0.0)               # unknown kind


# ---------------------------------------------------------------- session validation (A1 → A0)

def test_session_rejects_undeclared_param_index():
    s = wb.GraphSession()
    gn = s.add_node("gain", {"gain": 1.0})
    s.set_param(gn, 0, 0.5)                                   # gain has index 0 — ok
    with pytest.raises(ValueError):
        s.set_param(gn, 7, 0.5)                               # no such index → rejected


def test_validation_can_be_disabled():
    s = wb.GraphSession(validate=False)
    gn = s.add_node("gain", {"gain": 1.0})
    assert s.set_param(gn, 7, 0.5)                            # accepted when validation off


def test_undescribed_node_params_are_not_blocked():
    # a node whose params aren't described yet (e.g. channel_matrix) shouldn't hard-fail set_param
    s = wb.GraphSession()
    cm = s.add_node("channel_matrix", {"in_channels": 2, "out_channels": 2})
    assert s.set_param(cm, 0, 1.0)

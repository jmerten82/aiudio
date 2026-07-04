"""A0 — the graph-edit action space + session + serialization (Phase 2, ADR-0020).

The typed actions round-trip to JSON; applying them builds a real `Graph`; the graph serializes to a
document and back; the action log replays; undo/redo work. Pure aiudio core (no torch).
"""
from __future__ import annotations

import json

import pytest

from aiudio import workbench as wb


def _canon(doc: dict) -> str:
    """JSON-normalized form (tuples→lists, stable keys) for round-trip comparison."""
    return json.dumps(doc, sort_keys=True)


# ---------------------------------------------------------------- action space

def test_available_kinds_covers_the_library():
    kinds = set(wb.available_kinds())
    assert {"source", "sink", "gain", "compressor", "waveshaper", "neural_node"} <= kinds


@pytest.mark.parametrize("action", [
    wb.AddNode("gain", {"gain": 0.5}, (10.0, 20.0)),
    wb.AddNode("source"),
    wb.RemoveNode(3),
    wb.Connect(0, 0, 1, 0),
    wb.Disconnect(0, 0, 1, 0),
    wb.SetParam(2, 1, 4.0),
    wb.SetPosition(2, 10.0, 20.0),
])
def test_action_dict_roundtrip(action):
    assert wb.from_dict(wb.to_dict(action)) == action


def test_from_dict_rejects_unknown_op():
    with pytest.raises(ValueError):
        wb.from_dict({"op": "explode"})


# ---------------------------------------------------------------- applying to a graph

def test_apply_builds_a_graph():
    s = wb.GraphSession()
    src = s.add_node("source")
    gn = s.add_node("gain", {"gain": 0.5})
    snk = s.add_node("sink")
    s.connect(src, 0, gn, 0)
    s.connect(gn, 0, snk, 0)
    types = {nid: t for (nid, t, _i, _o) in s.graph.nodes()}
    assert types == {src: "SourceNode", gn: "GainNode", snk: "SinkNode"}
    assert set(s.graph.edges()) == {(src, 0, gn, 0), (gn, 0, snk, 0)}


def test_unknown_kind_raises():
    s = wb.GraphSession()
    with pytest.raises(ValueError):
        s.add_node("frobnicator")


def test_bad_connect_raises():
    s = wb.GraphSession()
    src = s.add_node("source")
    with pytest.raises(ValueError):
        s.connect(src, 0, 999, 0)          # nonexistent destination


# ---------------------------------------------------------------- serialization

def _channel_strip() -> wb.GraphSession:
    s = wb.GraphSession()
    src = s.add_node("source", position=(0.0, 0.0))
    ws = s.add_node("waveshaper", {"shape": "tanh", "drive": 2.0, "mix": 0.5}, (100.0, 0.0))
    cp = s.add_node("compressor", {"threshold_db": -24.0, "ratio": 4.0}, (200.0, 0.0))
    snk = s.add_node("sink", position=(300.0, 0.0))
    s.connect(src, 0, ws, 0)
    s.connect(ws, 0, cp, 0)
    s.connect(cp, 0, snk, 0)
    return s


def test_document_json_roundtrip():
    s = _channel_strip()
    doc = s.to_document()
    rebuilt = wb.GraphSession.from_json(s.to_json())
    assert _canon(doc) == _canon(rebuilt.to_document())          # identical document
    assert set(s.graph.edges()) == set(rebuilt.graph.edges())    # identical topology


def test_document_preserves_params_and_layout():
    s = wb.GraphSession()
    gn = s.add_node("gain", {"gain": 1.0}, (5.0, 6.0))
    s.set_param(gn, 0, 0.25)
    doc = wb.GraphSession.from_json(s.to_json()).to_document()
    node = doc["nodes"][0]
    assert node["params"] == {"0": 0.25}
    assert node["position"] == [5.0, 6.0]


def test_parametric_eq_bands_roundtrip():
    # bands is a list of tuples — survives the JSON (list) round-trip via tuple coercion on rebuild
    s = wb.GraphSession()
    s.add_node("parametric_eq", {"bands": [("peaking", 1000.0, 1.0, 3.0)], "sample_rate": 48000.0})
    rebuilt = wb.GraphSession.from_json(s.to_json())
    assert [t for (_i, t, *_r) in rebuilt.graph.nodes()] == ["ParametricEqNode"]


def test_log_replay_matches():
    s = _channel_strip()
    replayed = wb.GraphSession.from_log(s.log_to_list())
    assert _canon(s.to_document()) == _canon(replayed.to_document())


# ---------------------------------------------------------------- undo / redo / remove

def test_undo_redo():
    s = wb.GraphSession()
    s.add_node("source")
    s.add_node("gain", {"gain": 1.0})
    s.add_node("sink")
    assert len(s.to_document()["nodes"]) == 3
    assert s.undo() and len(s.to_document()["nodes"]) == 2
    assert s.redo() and len(s.to_document()["nodes"]) == 3
    # a fresh action clears the redo stack
    s.undo()
    s.add_node("meter")
    assert not s.redo()


def test_set_position_persists_in_the_document():
    s = wb.GraphSession()
    gn = s.add_node("gain", {"gain": 1.0})
    s.set_position(gn, 42.0, 7.0)
    node = s.to_document()["nodes"][0]
    assert node["position"] == [42.0, 7.0]
    # survives a JSON round-trip
    assert wb.GraphSession.from_json(s.to_json()).to_document()["nodes"][0]["position"] == [42.0, 7.0]


def test_remove_node_drops_it_from_the_document():
    s = wb.GraphSession()
    src = s.add_node("source")
    gn = s.add_node("gain", {"gain": 1.0})
    s.connect(src, 0, gn, 0)
    s.remove_node(gn)
    doc = s.to_document()
    assert [n["id"] for n in doc["nodes"]] == [src]
    assert doc["edges"] == []                                    # the edge went with the node

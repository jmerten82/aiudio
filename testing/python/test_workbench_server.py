"""A2 — the localhost workbench server (Phase 2, ADR-0019).

Headless tests of the FastAPI/WebSocket bridge via the in-process ASGI test client (no browser, no
real server): HTTP manifest/graph endpoints, and the WebSocket protocol — initial state, apply
action + broadcast, undo, error handling, and two clients sharing one authoritative graph.
Gated on the workbench extra.
"""
from __future__ import annotations

import pytest

pytest.importorskip("fastapi")
pytest.importorskip("httpx")                       # backs starlette's TestClient
from fastapi.testclient import TestClient           # noqa: E402

from aiudio.server import create_app                # noqa: E402

ADD_GAIN = {"type": "action", "action": {"op": "add_node", "node": "gain", "args": {"gain": 0.5}}}


def _client() -> TestClient:
    return TestClient(create_app())


def _drain_initial(ws) -> None:
    assert ws.receive_json()["type"] == "manifest"
    assert ws.receive_json()["type"] == "graph"


# ---------------------------------------------------------------- HTTP

def test_health_manifest_graph_endpoints():
    c = _client()
    assert c.get("/api/health").json()["status"] == "ok"
    assert "gain" in c.get("/api/manifest").json()["kinds"]
    assert c.get("/api/graph").json() == {"nodes": [], "edges": []}


# ---------------------------------------------------------------- WebSocket

def test_ws_sends_initial_manifest_and_graph():
    with _client().websocket_connect("/ws") as ws:
        assert ws.receive_json()["type"] == "manifest"
        graph = ws.receive_json()
        assert graph["type"] == "graph" and graph["doc"]["nodes"] == []


def test_ws_apply_action_acks_and_broadcasts():
    with _client().websocket_connect("/ws") as ws:
        _drain_initial(ws)
        ws.send_json(ADD_GAIN)
        ack = ws.receive_json()
        assert ack["type"] == "result" and isinstance(ack["value"], int)     # the new node id
        state = ws.receive_json()
        assert state["type"] == "graph"
        assert [n["node"] for n in state["doc"]["nodes"]] == ["gain"]


def test_ws_undo():
    with _client().websocket_connect("/ws") as ws:
        _drain_initial(ws)
        ws.send_json(ADD_GAIN)
        ws.receive_json()                                                     # result
        ws.receive_json()                                                     # graph (1 node)
        ws.send_json({"type": "undo"})
        ws.receive_json()                                                     # result
        assert ws.receive_json()["doc"]["nodes"] == []                        # back to empty


def test_ws_bad_action_errors_without_mutating():
    with _client().websocket_connect("/ws") as ws:
        _drain_initial(ws)
        ws.send_json({"type": "action", "action": {"op": "add_node", "node": "frobnicator"}})
        err = ws.receive_json()
        assert err["type"] == "error" and "frobnicator" in err["message"]
        # session unchanged: a follow-up sync shows an empty graph
        ws.send_json({"type": "sync"})
        assert ws.receive_json()["doc"]["nodes"] == []


def test_serves_static_frontend_when_given(tmp_path):
    (tmp_path / "index.html").write_text("<!doctype html><title>aiudio</title>")
    client = TestClient(create_app(static_dir=str(tmp_path)))
    assert client.get("/api/health").json()["status"] == "ok"      # API still wins
    root = client.get("/")
    assert root.status_code == 200 and "aiudio" in root.text        # SPA served at /


def test_ws_load_replaces_the_graph():
    with _client().websocket_connect("/ws") as ws:
        _drain_initial(ws)
        doc = {"nodes": [{"id": 0, "node": "gain", "args": {"gain": 0.5}, "params": {}, "position": [1, 2]}],
               "edges": []}
        ws.send_json({"type": "load", "doc": doc})
        state = ws.receive_json()
        assert state["type"] == "graph"
        assert [n["node"] for n in state["doc"]["nodes"]] == ["gain"]
        assert state["doc"]["nodes"][0]["position"] == [1, 2]


def test_two_clients_share_one_graph():
    app = create_app()
    client = TestClient(app)
    with client.websocket_connect("/ws") as a_ws, client.websocket_connect("/ws") as b_ws:
        _drain_initial(a_ws)
        _drain_initial(b_ws)
        a_ws.send_json(ADD_GAIN)
        assert a_ws.receive_json()["type"] == "result"       # ack to the acting client
        # both clients receive the broadcast of the new authoritative state
        assert len(a_ws.receive_json()["doc"]["nodes"]) == 1
        assert len(b_ws.receive_json()["doc"]["nodes"]) == 1

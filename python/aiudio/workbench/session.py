"""`GraphSession` — apply graph-edit actions to a `Graph`, with an append-only log (Phase 2 · A0).

The editable-IR half of the workbench: actions (`actions.py`) are applied onto the C++ `Graph`
(the same API the code path uses), recorded in an append-only log (undo/redo/replay), and the whole
graph serializes to/from a JSON **document** (`{nodes, edges}` with UI layout) — the form the
browser renders (React Flow) and the wire transports (ADR-0020).

Scope note: this operates on the `Graph` (topology + construction args). `set_param` records the
intended value in the node's record; *applying* params to a running engine (the lock-free queue) is
milestone A2. Manifest-based validation of kinds/param ranges arrives with A1 (ADR-0021); A0 does
structural validation (kind exists, connect succeeds).
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field

import aiudio

from .actions import (
    Action,
    AddNode,
    Connect,
    Disconnect,
    RemoveNode,
    SetParam,
    from_dict,
    to_dict,
)


@dataclass
class NodeRecord:
    kind: str                                  # factory kind (Graph.add_<kind>)
    args: dict                                 # constructor kwargs
    params: dict[int, float] = field(default_factory=dict)   # runtime param overrides (index→value)
    position: tuple[float, float] | None = None              # UI layout


def _coerce(value):
    """JSON turns tuples into lists; a few factories (e.g. parametric_eq bands) want tuples."""
    if isinstance(value, list) and value and all(isinstance(e, list) for e in value):
        return [tuple(e) for e in value]
    return value


def available_kinds() -> list[str]:
    """Factory kinds the current build exposes (``Graph.add_<kind>``). A pragmatic A0 stand-in for
    the full capability manifest (A1/ADR-0021)."""
    return sorted(m[4:] for m in dir(aiudio.Graph) if m.startswith("add_"))


class GraphSession:
    """A live `Graph` plus the action log that built it."""

    def __init__(self) -> None:
        self.graph = aiudio.Graph()
        self._nodes: dict[int, NodeRecord] = {}
        self.log: list[Action] = []
        self._redo: list[Action] = []

    # ---- apply ------------------------------------------------------------------ #

    def apply(self, action: Action):
        """Apply an action, record it in the log, and clear the redo stack."""
        result = self._exec(action)
        self.log.append(action)
        self._redo.clear()
        return result

    def _exec(self, action: Action):
        if isinstance(action, AddNode):
            factory = "add_" + action.node
            if not hasattr(self.graph, factory):
                raise ValueError(f"unknown node kind: {action.node!r}")
            args = {k: _coerce(v) for k, v in action.args.items()}
            nid = getattr(self.graph, factory)(**args)
            self._nodes[nid] = NodeRecord(action.node, dict(action.args), {}, action.position)
            return nid
        if isinstance(action, RemoveNode):
            if not self.graph.remove_node(action.id):
                raise ValueError(f"remove_node failed for id {action.id}")
            self._nodes.pop(action.id, None)
            return True
        if isinstance(action, Connect):
            if not self.graph.connect(action.src, action.src_port, action.dst, action.dst_port):
                raise ValueError(f"connect failed: {action}")
            return True
        if isinstance(action, Disconnect):
            return self.graph.disconnect(action.src, action.src_port, action.dst, action.dst_port)
        if isinstance(action, SetParam):
            record = self._nodes.get(action.node)
            if record is None:
                raise ValueError(f"set_param on unknown node {action.node}")
            record.params[action.index] = action.value
            return True
        raise TypeError(f"unknown action: {action!r}")

    # ---- convenience wrappers ---------------------------------------------------- #

    def add_node(self, kind: str, args: dict | None = None,
                 position: tuple[float, float] | None = None) -> int:
        return self.apply(AddNode(kind, dict(args or {}), position))

    def connect(self, src: int, src_port: int, dst: int, dst_port: int) -> bool:
        return self.apply(Connect(src, src_port, dst, dst_port))

    def disconnect(self, src: int, src_port: int, dst: int, dst_port: int) -> bool:
        return self.apply(Disconnect(src, src_port, dst, dst_port))

    def remove_node(self, node_id: int) -> bool:
        return self.apply(RemoveNode(node_id))

    def set_param(self, node: int, index: int, value: float) -> bool:
        return self.apply(SetParam(node, index, value))

    # ---- undo / redo / replay ---------------------------------------------------- #

    def undo(self) -> bool:
        """Undo the last action by replaying the log without it (ids reproduce deterministically)."""
        if not self.log:
            return False
        self._redo.append(self.log.pop())
        self._rebuild()
        return True

    def redo(self) -> bool:
        if not self._redo:
            return False
        action = self._redo.pop()
        self._exec(action)
        self.log.append(action)
        return True

    def _rebuild(self) -> None:
        self.graph = aiudio.Graph()
        self._nodes = {}
        for action in self.log:
            self._exec(action)

    # ---- serialization: graph document (what the UI renders) --------------------- #

    def to_document(self) -> dict:
        """Serialize to ``{nodes: [...], edges: [...]}`` (with args, param overrides, UI layout)."""
        nodes = []
        for nid, record in self._nodes.items():
            if self.graph.node_type(nid) is None:            # skip removed/tombstoned ids
                continue
            nodes.append({
                "id": nid,
                "node": record.kind,
                "args": record.args,
                "params": {str(k): v for k, v in record.params.items()},
                "position": list(record.position) if record.position else None,
            })
        edges = [{"src": s, "src_port": sp, "dst": d, "dst_port": dp}
                 for (s, sp, d, dp) in self.graph.edges()]
        return {"nodes": nodes, "edges": edges}

    @classmethod
    def from_document(cls, doc: dict) -> "GraphSession":
        """Rebuild a session from a document (ids are remapped, so topology is preserved even if the
        source had removed/tombstoned ids)."""
        session = cls()
        idmap: dict[int, int] = {}
        for n in doc.get("nodes", []):
            position = tuple(n["position"]) if n.get("position") else None
            new_id = session.add_node(n["node"], dict(n.get("args", {})), position)
            idmap[n["id"]] = new_id
            for index, value in (n.get("params") or {}).items():
                session.set_param(new_id, int(index), value)
        for e in doc.get("edges", []):
            session.connect(idmap[e["src"]], e["src_port"], idmap[e["dst"]], e["dst_port"])
        return session

    def to_json(self) -> str:
        return json.dumps(self.to_document())

    @classmethod
    def from_json(cls, text: str) -> "GraphSession":
        return cls.from_document(json.loads(text))

    # ---- serialization: action log (history / replay) ---------------------------- #

    def log_to_list(self) -> list[dict]:
        return [to_dict(a) for a in self.log]

    @classmethod
    def from_log(cls, actions: list[dict]) -> "GraphSession":
        session = cls()
        for d in actions:
            session.apply(from_dict(d))
        return session

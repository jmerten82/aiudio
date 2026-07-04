"""The typed graph-edit action space (Phase 2 · A0, ADR-0020).

One set of actions is the substrate for three consumers — the hand editor (UI), the agent, and the
wire protocol — plus an append-only log (undo/replay). Each action is a small, frozen, JSON-round-
trippable record; `to_dict`/`from_dict` are the wire form. Actions are *applied* by a
`GraphSession` (see `session.py`), which maps them onto the C++ `Graph`.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass, field
from typing import ClassVar, Union


@dataclass(frozen=True)
class AddNode:
    """Add a node of factory kind ``node`` (e.g. ``"gain"``, ``"compressor"``) with constructor
    ``args`` (kwargs of the matching ``Graph.add_<node>``). ``position`` is optional UI layout."""

    node: str
    args: dict = field(default_factory=dict)
    position: tuple[float, float] | None = None
    OP: ClassVar[str] = "add_node"


@dataclass(frozen=True)
class RemoveNode:
    id: int
    OP: ClassVar[str] = "remove_node"


@dataclass(frozen=True)
class Connect:
    src: int
    src_port: int
    dst: int
    dst_port: int
    OP: ClassVar[str] = "connect"


@dataclass(frozen=True)
class Disconnect:
    src: int
    src_port: int
    dst: int
    dst_port: int
    OP: ClassVar[str] = "disconnect"


@dataclass(frozen=True)
class SetParam:
    node: int
    index: int
    value: float
    OP: ClassVar[str] = "set_param"


@dataclass(frozen=True)
class SetPosition:
    """UI-layout only: where a node sits on the canvas. Persisted in the document (B2) so a layout
    survives reload and is shared across clients; it has no effect on the audio graph."""

    id: int
    x: float
    y: float
    OP: ClassVar[str] = "set_position"


Action = Union[AddNode, RemoveNode, Connect, Disconnect, SetParam, SetPosition]

_CLASSES: dict[str, type] = {
    c.OP: c for c in (AddNode, RemoveNode, Connect, Disconnect, SetParam, SetPosition)
}


def to_dict(action: Action) -> dict:
    """Serialize an action to a JSON-safe dict tagged with its ``op``."""
    d: dict = {"op": action.OP}
    for k, v in asdict(action).items():
        d[k] = list(v) if isinstance(v, tuple) else v
    return d


def from_dict(d: dict) -> Action:
    """Parse an action dict (inverse of :func:`to_dict`)."""
    d = dict(d)
    op = d.pop("op")
    cls = _CLASSES.get(op)
    if cls is None:
        raise ValueError(f"unknown action op: {op!r}")
    if op == AddNode.OP and d.get("position") is not None:
        d["position"] = tuple(d["position"])
    return cls(**d)

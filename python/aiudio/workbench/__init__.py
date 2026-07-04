"""aiudio.workbench — the Phase 2 workbench substrate (agent control plane + visual editor).

The **typed graph-edit action space** and graph↔JSON serialization (Phase 2 · A0, ADR-0020): one
set of actions shared by the hand editor, the agent, and the wire protocol, applied to a `Graph`
via a `GraphSession` with an append-only log (undo/redo/replay). This is the frontend/backend
*contract*; the server bridge (A2) and the capability manifest (A1) build on it.

Pure-Python, no torch — part of the base package.
"""
from __future__ import annotations

from .actions import (
    Action,
    AddNode,
    Connect,
    Disconnect,
    RemoveNode,
    SetParam,
    SetPosition,
    from_dict,
    to_dict,
)
from .manifest import capability_manifest, node_manifest, param_issues
from .session import GraphSession, NodeRecord, available_kinds
from .tuning import tune_to_target

__all__ = [
    "Action",
    "AddNode",
    "RemoveNode",
    "Connect",
    "Disconnect",
    "SetParam",
    "SetPosition",
    "to_dict",
    "from_dict",
    "GraphSession",
    "NodeRecord",
    "available_kinds",
    "capability_manifest",
    "node_manifest",
    "param_issues",
    "tune_to_target",
]

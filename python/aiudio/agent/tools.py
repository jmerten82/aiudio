"""Agent tools = the graph-edit action space (Phase 2 · C0, ADR-0022).

The agent's tools are exactly the workbench actions (ADR-0020), and their schema is **grounded in
the capability manifest** (ADR-0021) — `add_node`'s ``node`` is an enum of the real factory kinds,
so the agent can't invent nodes that don't exist. `apply_tool` maps a tool call onto a
`GraphSession` (the same path the UI uses), so the human editor and the agent share one graph + log.
"""
from __future__ import annotations

from typing import Any

from aiudio import workbench as wb

# Tool names that only *read* — never mutate the graph (so the consent policy can ignore them).
READ_ONLY = frozenset({"get_graph"})


def action_tools(manifest: dict | None = None) -> list[dict]:
    """The Anthropic tool schema for the action space, grounded in the manifest."""
    kinds = sorted((manifest or wb.capability_manifest())["kinds"])
    return [
        {
            "name": "add_node",
            "description": "Add a node of the given kind (with optional constructor args + canvas "
                           "position). Returns the new node id.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "node": {"type": "string", "enum": kinds, "description": "the node kind"},
                    "args": {"type": "object", "description": "constructor args (see the kind's defaults)"},
                    "position": {"type": "array", "items": {"type": "number"}, "description": "[x, y]"},
                },
                "required": ["node"],
            },
        },
        {
            "name": "remove_node",
            "description": "Remove a node (and its edges) by id.",
            "input_schema": {"type": "object", "properties": {"id": {"type": "integer"}},
                             "required": ["id"]},
        },
        {
            "name": "connect",
            "description": "Connect src:src_port → dst:dst_port.",
            "input_schema": {"type": "object", "properties": {
                "src": {"type": "integer"}, "src_port": {"type": "integer"},
                "dst": {"type": "integer"}, "dst_port": {"type": "integer"}},
                "required": ["src", "src_port", "dst", "dst_port"]},
        },
        {
            "name": "disconnect",
            "description": "Remove the edge src:src_port → dst:dst_port.",
            "input_schema": {"type": "object", "properties": {
                "src": {"type": "integer"}, "src_port": {"type": "integer"},
                "dst": {"type": "integer"}, "dst_port": {"type": "integer"}},
                "required": ["src", "src_port", "dst", "dst_port"]},
        },
        {
            "name": "set_param",
            "description": "Set parameter `index` of a node to `value` (see the manifest for indices "
                           "and ranges).",
            "input_schema": {"type": "object", "properties": {
                "node": {"type": "integer"}, "index": {"type": "integer"}, "value": {"type": "number"}},
                "required": ["node", "index", "value"]},
        },
        {
            "name": "get_graph",
            "description": "Read the current graph document ({nodes, edges}).",
            "input_schema": {"type": "object", "properties": {}},
        },
    ]


def apply_tool(session: wb.GraphSession, name: str, tool_input: dict) -> Any:
    """Apply one tool call to the session (same actions the UI uses). Raises on a bad call."""
    if name == "add_node":
        position = tool_input.get("position")
        node_id = session.add_node(
            tool_input["node"], dict(tool_input.get("args", {})),
            tuple(position) if position else None)
        return {"id": node_id}
    if name == "remove_node":
        return {"ok": session.remove_node(int(tool_input["id"]))}
    if name == "connect":
        return {"ok": session.connect(int(tool_input["src"]), int(tool_input["src_port"]),
                                      int(tool_input["dst"]), int(tool_input["dst_port"]))}
    if name == "disconnect":
        return {"ok": session.disconnect(int(tool_input["src"]), int(tool_input["src_port"]),
                                         int(tool_input["dst"]), int(tool_input["dst_port"]))}
    if name == "set_param":
        return {"ok": session.set_param(int(tool_input["node"]), int(tool_input["index"]),
                                        float(tool_input["value"]))}
    if name == "get_graph":
        return session.to_document()
    raise ValueError(f"unknown tool: {name!r}")

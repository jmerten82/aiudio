"""The grounded agent runtime (Phase 2 · C0, ADR-0022).

Claude tool-use bound to the action-space tools (`tools.py`), grounded by the capability manifest
(the system prompt lists the real node catalog), applying tool calls to a `GraphSession`. The LLM is
behind a tiny `create(...)`-shaped seam so the loop is unit-tested with a mock client (no API key);
`AnthropicClient` is the real one. Human-in-the-loop: an `is_invasive` classifier + an `on_invasive`
approval callback gate RT-invasive changes (ADR-0022) — the mechanism the UI (C1) will surface.

Messages/content blocks use Anthropic's JSON shape directly (text / tool_use / tool_result dicts),
so the mock is trivial and the real client just converts its response objects to dicts.
"""
from __future__ import annotations

import json
from collections.abc import Callable
from dataclasses import dataclass, field

from aiudio import workbench as wb

from .tools import READ_ONLY, action_tools, apply_tool

DEFAULT_MODEL = "claude-sonnet-4-6"


def build_system_prompt(manifest: dict) -> str:
    """A compact grounding prompt: the real node kinds + their parameter indices/ranges."""
    lines = [
        "You edit an aiudio audio-DSP graph by calling tools (the graph-edit action space).",
        "Use ONLY node kinds and parameter indices that exist below; inspect with get_graph.",
        "Signal flows source → … → sink. Be concise; explain what you changed.",
        "",
        "Node kinds (kind (Type, in→out): index=name[min..max]unit …):",
    ]
    for kind, m in sorted(manifest["kinds"].items()):
        params = ", ".join(
            f"{p['index']}={p['name']}[{p['min']}..{p['max']}]{p['unit']}" for p in m["params"])
        lines.append(f"- {kind} ({m['type']}, {m['num_inputs']}→{m['num_outputs']})"
                     f"{': ' + params if params else ''}")
    return "\n".join(lines)


@dataclass
class AgentResult:
    text: str                                   # the agent's final natural-language reply
    applied: list[dict] = field(default_factory=list)   # mutating tool calls that were applied
    transcript: list[dict] = field(default_factory=list)  # the full message list


class Agent:
    """Drives a natural-language request into graph edits via tool-use over a `GraphSession`."""

    def __init__(self, session: wb.GraphSession, client, *, manifest: dict | None = None,
                 model: str = DEFAULT_MODEL, max_tokens: int = 2048,
                 is_invasive: Callable[[str, dict], bool] | None = None,
                 on_invasive: Callable[[str, dict], bool] | None = None,
                 system: str | None = None) -> None:
        self.session = session
        self.client = client
        self.manifest = manifest or wb.capability_manifest()
        self.tools = action_tools(self.manifest)
        self.model = model
        self.max_tokens = max_tokens
        # default: nothing is invasive (an offline session has no live audio thread — ADR-0022).
        self.is_invasive = is_invasive or (lambda name, tool_input: False)
        self.on_invasive = on_invasive
        self.system = system or build_system_prompt(self.manifest)

    def run(self, message: str, max_turns: int = 8) -> AgentResult:
        messages: list[dict] = [{"role": "user", "content": [{"type": "text", "text": message}]}]
        applied: list[dict] = []
        for _ in range(max_turns):
            resp = self.client.create(system=self.system, messages=messages, tools=self.tools,
                                      model=self.model, max_tokens=self.max_tokens)
            content = resp["content"]
            tool_uses = [b for b in content if b.get("type") == "tool_use"]
            if not tool_uses:
                text = " ".join(b["text"] for b in content if b.get("type") == "text")
                return AgentResult(text=text.strip(), applied=applied, transcript=messages)
            messages.append({"role": "assistant", "content": content})
            results = []
            for tu in tool_uses:
                results.append(self._run_tool(tu, applied))
            messages.append({"role": "user", "content": results})
        return AgentResult(text="(stopped: max turns reached)", applied=applied, transcript=messages)

    def _run_tool(self, tool_use: dict, applied: list[dict]) -> dict:
        name, tool_input, tid = tool_use["name"], tool_use.get("input", {}), tool_use["id"]
        result = {"type": "tool_result", "tool_use_id": tid}
        # RT-invasive changes require explicit approval (ADR-0022 §5.1a).
        if name not in READ_ONLY and self.is_invasive(name, tool_input):
            if self.on_invasive is None or not self.on_invasive(name, tool_input):
                return {**result, "content": "declined: this change needs user confirmation",
                        "is_error": True}
        try:
            out = apply_tool(self.session, name, tool_input)
            if name not in READ_ONLY:
                applied.append({"name": name, "input": tool_input})
            return {**result, "content": json.dumps(out)}
        except Exception as exc:  # noqa: BLE001 - report the error back to the model, keep going
            return {**result, "content": f"error: {exc}", "is_error": True}


class AnthropicClient:
    """The real LLM client — wraps the Anthropic SDK. Requires the `agent` extra
    (`pip install "aiudio[agent]"`) and an API key. Converts responses to the dict block shape."""

    def __init__(self, api_key: str | None = None, model: str = DEFAULT_MODEL) -> None:
        import anthropic  # lazy: only the real client needs the SDK

        self._client = anthropic.Anthropic(api_key=api_key) if api_key else anthropic.Anthropic()
        self.model = model

    def create(self, *, system, messages, tools, model=None, max_tokens=2048) -> dict:
        msg = self._client.messages.create(model=model or self.model, system=system,
                                            messages=messages, tools=tools, max_tokens=max_tokens)
        content: list[dict] = []
        for block in msg.content:
            if block.type == "text":
                content.append({"type": "text", "text": block.text})
            elif block.type == "tool_use":
                content.append({"type": "tool_use", "id": block.id, "name": block.name,
                                "input": block.input})
        return {"stop_reason": msg.stop_reason, "content": content}

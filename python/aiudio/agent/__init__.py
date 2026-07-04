"""aiudio.agent — the grounded LLM control plane (Phase 2 · C0, ADR-0022).

Claude tool-use over the graph-edit action space (ADR-0020), grounded by the capability manifest
(ADR-0021), applying edits to a `GraphSession`. The tool-use loop is decoupled from the LLM by a
small client seam, so it's unit-testable with a mock; `AnthropicClient` is the real one.

The runtime is pure Python; only `AnthropicClient` needs the SDK — `pip install "aiudio[agent]"`.
"""
from __future__ import annotations

from .runtime import Agent, AgentResult, AnthropicClient, build_system_prompt
from .tools import action_tools, apply_tool

__all__ = [
    "Agent",
    "AgentResult",
    "AnthropicClient",
    "build_system_prompt",
    "action_tools",
    "apply_tool",
]

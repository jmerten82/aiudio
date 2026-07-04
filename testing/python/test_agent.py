"""C0 — the grounded agent runtime (Phase 2, ADR-0022).

The tool-use loop is tested with a mock LLM client (no API key): tools are grounded in the manifest,
scripted tool calls apply to the session, the consent gate blocks/allows RT-invasive changes, and
tool errors are reported back rather than crashing. A live test (real Claude) is gated on
ANTHROPIC_API_KEY and skips in CI.
"""
from __future__ import annotations

import os

import pytest

from aiudio import workbench as wb
from aiudio.agent import Agent, action_tools, apply_tool, build_system_prompt


# ---- a scripted mock client (returns pre-baked responses in order) ----
class MockClient:
    def __init__(self, responses):
        self._responses = list(responses)
        self.calls = []

    def create(self, **kwargs):
        self.calls.append(kwargs)
        return self._responses.pop(0)


def _tool_use(name, tool_input, tid="t"):
    return {"type": "tool_use", "id": tid, "name": name, "input": tool_input}


def _resp_tools(*blocks):
    return {"stop_reason": "tool_use", "content": list(blocks)}


def _resp_text(text):
    return {"stop_reason": "end_turn", "content": [{"type": "text", "text": text}]}


# ---------------------------------------------------------------- grounding + tools

def test_tools_are_grounded_in_the_manifest():
    tools = {t["name"]: t for t in action_tools()}
    assert {"add_node", "connect", "set_param", "get_graph"} <= set(tools)
    node_enum = tools["add_node"]["input_schema"]["properties"]["node"]["enum"]
    assert set(node_enum) == set(wb.available_kinds())          # can't invent nonexistent kinds


def test_system_prompt_lists_real_nodes_and_params():
    prompt = build_system_prompt(wb.capability_manifest())
    assert "gain" in prompt and "compressor" in prompt
    assert "threshold_db" in prompt                            # param names are grounded in


def test_apply_tool_maps_to_session():
    s = wb.GraphSession()
    src = apply_tool(s, "add_node", {"node": "source"})["id"]
    gain = apply_tool(s, "add_node", {"node": "gain", "args": {"gain": 1.0}})["id"]
    apply_tool(s, "connect", {"src": src, "src_port": 0, "dst": gain, "dst_port": 0})
    assert apply_tool(s, "get_graph", {})["edges"] == [{"src": src, "src_port": 0, "dst": gain, "dst_port": 0}]


# ---------------------------------------------------------------- the loop

def test_agent_builds_a_graph_from_tool_calls():
    s = wb.GraphSession()
    client = MockClient([
        _resp_tools(_tool_use("add_node", {"node": "source"}, "a"),
                    _tool_use("add_node", {"node": "gain", "args": {"gain": 1.0}}, "b")),
        _resp_tools(_tool_use("connect", {"src": 0, "src_port": 0, "dst": 1, "dst_port": 0}, "c")),
        _resp_text("Added a gain after the source."),
    ])
    result = Agent(s, client).run("add a gain after a source")
    assert result.text == "Added a gain after the source."
    assert {t for (_i, t, *_r) in s.graph.nodes()} == {"SourceNode", "GainNode"}
    assert (0, 0, 1, 0) in set(s.graph.edges())
    assert len(result.applied) == 3


def test_tool_error_is_reported_not_raised():
    s = wb.GraphSession()
    client = MockClient([
        _resp_tools(_tool_use("add_node", {"node": "frobnicator"}, "x")),  # invalid kind → error
        _resp_text("Sorry, that node doesn't exist."),
    ])
    result = Agent(s, client).run("add a frobnicator")
    assert result.text.startswith("Sorry")
    assert s.to_document()["nodes"] == []                      # nothing added
    # the error was fed back to the model as a tool_result
    tool_results = client.calls[1]["messages"][-1]["content"]
    assert tool_results[0]["is_error"] is True


# ---------------------------------------------------------------- consent gate (ADR-0022)

def _prep():
    s = wb.GraphSession()
    s.add_node("source")
    s.add_node("gain", {"gain": 1.0})
    client = MockClient([_resp_tools(_tool_use("remove_node", {"id": 1}, "r")), _resp_text("done")])
    return s, client


def test_invasive_change_declined_when_not_approved():
    s, client = _prep()
    Agent(s, client, is_invasive=lambda n, i: n == "remove_node", on_invasive=lambda n, i: False).run("remove the gain")
    assert s.graph.node_type(1) == "GainNode"                  # still there — declined


def test_invasive_change_applied_when_approved():
    s, client = _prep()
    Agent(s, client, is_invasive=lambda n, i: True, on_invasive=lambda n, i: True).run("remove the gain")
    assert s.graph.node_type(1) is None                        # removed — approved


# ---------------------------------------------------------------- live (real Claude, opt-in)

@pytest.mark.skipif(not os.environ.get("ANTHROPIC_API_KEY"),
                    reason="live agent test — set ANTHROPIC_API_KEY to run against real Claude")
def test_live_agent_adds_a_gain():
    from aiudio.agent import AnthropicClient

    s = wb.GraphSession()
    Agent(s, AnthropicClient()).run("Add a source, then a gain node set to 0.5, and connect them.")
    kinds = {t for (_i, t, *_r) in s.graph.nodes()}
    assert "GainNode" in kinds

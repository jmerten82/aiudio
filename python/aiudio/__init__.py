"""aiudio — AI-native digital audio processing framework (Python bindings).

Thin Python layer over the C++ graph engine. The real-time core stays in C++
(ADR-0002); this exposes the graph IR, the executor (with numpy I/O), and the
offline file backend so the research/ML layer and the agent can build, run, and
edit graphs.
"""

from ._aiudio import Graph, GraphExecutor, OfflineBackend  # noqa: F401

__all__ = ["Graph", "GraphExecutor", "OfflineBackend"]
__version__ = "0.0.1"

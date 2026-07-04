"""aiudio.server — the Phase 2 localhost workbench bridge (ADR-0019).

A FastAPI + WebSocket server that hosts the engine and speaks the graph-edit action space
(ADR-0020) to a browser frontend. Run it with ``python -m aiudio.server`` (or `serve()`).

Optional — requires the workbench extra: ``pip install "aiudio[workbench]"``. FastAPI is imported
lazily in `app`, so importing this module without the extra raises a clear install hint.
"""
from __future__ import annotations

from .app import ConnectionManager, create_app, serve

__all__ = ["create_app", "serve", "ConnectionManager"]

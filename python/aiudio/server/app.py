"""The localhost workbench server (Phase 2 · A2, ADR-0019).

A FastAPI + WebSocket bridge that hosts one authoritative `GraphSession` (A0) and speaks the
graph-edit action space (ADR-0020) to the browser: HTTP for the capability manifest + current
graph document, and a WebSocket that applies actions and broadcasts the resulting state to every
connected client (so a human editor and the agent collaborate on one graph). The browser is a
control frontend only — this process never runs audio, and the audio thread never blocks on it.

Optional — requires the workbench extra: ``pip install "aiudio[workbench]"``.
"""
from __future__ import annotations

import asyncio

try:
    from fastapi import FastAPI, WebSocket, WebSocketDisconnect
except ModuleNotFoundError as exc:  # pragma: no cover - exercised only without the extra
    raise ModuleNotFoundError(
        'aiudio.server requires the workbench extra — install it with: '
        'pip install "aiudio[workbench]"'
    ) from exc

from aiudio import workbench as wb


class ConnectionManager:
    """Tracks connected WebSocket clients and broadcasts state to all of them."""

    def __init__(self) -> None:
        self._clients: set = set()

    async def connect(self, ws: WebSocket) -> None:
        await ws.accept()
        self._clients.add(ws)

    def disconnect(self, ws: WebSocket) -> None:
        self._clients.discard(ws)

    async def broadcast(self, message: dict) -> None:
        for ws in list(self._clients):
            try:
                await ws.send_json(message)
            except Exception:  # noqa: BLE001 - a dead socket shouldn't break the broadcast
                self._clients.discard(ws)

    @property
    def count(self) -> int:
        return len(self._clients)


def create_app(session: wb.GraphSession | None = None, static_dir: str | None = None) -> "FastAPI":
    """Build the workbench app around an authoritative `GraphSession` (a fresh one by default).

    If ``static_dir`` points at a built frontend (``web/dist``), it is served at ``/`` so the whole
    workbench runs from this process. In dev, skip it — the Vite dev server serves the UI and
    proxies ``/api``/``/ws`` here.
    """
    app = FastAPI(title="aiudio workbench", version="0.2")
    app.state.session = session or wb.GraphSession()
    app.state.manager = ConnectionManager()
    app.state.lock = asyncio.Lock()

    @app.get("/api/health")
    async def health() -> dict:
        return {"status": "ok"}

    @app.get("/api/manifest")
    async def manifest() -> dict:
        return wb.capability_manifest()

    @app.get("/api/graph")
    async def graph() -> dict:
        return app.state.session.to_document()

    def _handle(message: dict):
        """Apply one client message to the session; return an ack payload or None. Raises on a bad
        message (the caller turns that into an error reply, leaving the session unchanged)."""
        session = app.state.session
        kind = message.get("type")
        if kind == "action":
            return {"type": "result", "value": session.apply(wb.from_dict(message["action"]))}
        if kind == "undo":
            return {"type": "result", "value": session.undo()}
        if kind == "redo":
            return {"type": "result", "value": session.redo()}
        if kind == "load":
            # replace the whole graph from a saved document (B2 save/load)
            app.state.session = wb.GraphSession.from_document(message["doc"])
            return None
        if kind == "sync":
            return None
        raise ValueError(f"unknown message type: {kind!r}")

    @app.websocket("/ws")
    async def ws_endpoint(ws: WebSocket) -> None:
        manager = app.state.manager
        await manager.connect(ws)
        await ws.send_json({"type": "manifest", "manifest": wb.capability_manifest()})
        await ws.send_json({"type": "graph", "doc": app.state.session.to_document()})
        try:
            while True:
                message = await ws.receive_json()
                try:
                    async with app.state.lock:
                        ack = _handle(message)
                except (ValueError, KeyError, TypeError) as exc:
                    await ws.send_json({"type": "error", "message": str(exc)})
                    continue
                if ack is not None:
                    await ws.send_json(ack)
                await manager.broadcast({"type": "graph", "doc": app.state.session.to_document()})
        except WebSocketDisconnect:
            manager.disconnect(ws)

    # Serve the built SPA last, so the API/ws routes above take precedence (mount at "/" is a catch-all).
    if static_dir is not None:
        from pathlib import Path

        from fastapi.staticfiles import StaticFiles

        path = Path(static_dir)
        if path.is_dir():
            app.mount("/", StaticFiles(directory=str(path), html=True), name="web")

    return app


def serve(host: str = "127.0.0.1", port: int = 8765, static_dir: str | None = None) -> None:
    """Run the workbench server (blocking) via uvicorn."""
    import uvicorn

    uvicorn.run(create_app(static_dir=static_dir), host=host, port=port)

# aiudio workbench (web)

The Phase 2 **visual workbench** frontend — a React + React Flow view of the live aiudio graph
(ADR-0019). It's a control frontend only: it renders the graph the [server](../python/aiudio/server)
broadcasts over WebSocket and never touches audio.

**B0 (this milestone) is read-only** — it *sees* the graph (nodes, ports, edges, current parameter
values from the capability manifest) and reconciles from the server's broadcasts. Hand editing is
B1; the agent companion is workstream C.

## Develop

```bash
# 1. run the backend (a separate terminal) — needs the workbench extra
pip install -e ".[workbench]"
python -m aiudio.server                 # localhost:8765

# 2. run the frontend dev server (proxies /api + /ws → :8765)
cd web
npm install
npm run dev                             # open the printed localhost URL
```

## Verify / build

```bash
npm run typecheck    # tsc --noEmit
npm test             # vitest (the pure document→flow transform)
npm run build        # → web/dist
```

## Serve the built app from the backend

```bash
npm run build
python -m aiudio.server --static web/dist    # whole workbench on localhost:8765
```

## Layout
- `src/types.ts` — wire types (graph document, manifest, actions) mirroring the backend.
- `src/graph.ts` — the pure `documentToFlow` transform (unit-tested in `graph.test.ts`).
- `src/ws.ts` — the WebSocket client.
- `src/GraphView.tsx` — the custom React Flow node (title + read-only params + port handles).
- `src/App.tsx` — connects, holds manifest + graph state, renders the canvas.

// Pure transform: an aiudio graph document (backend) → React Flow nodes + edges. No React, no DOM —
// so it's unit-testable on its own (graph.test.ts). Nodes without a saved layout get a simple
// left-to-right fallback position.
import type { Edge, Node } from '@xyflow/react'

import type { GraphDocument, Manifest, NodeManifest } from './types'

export interface AiudioNodeData extends Record<string, unknown> {
  node: string
  label: string
  params: Record<string, number>
  manifest: NodeManifest | null
}

export function documentToFlow(
  doc: GraphDocument,
  manifest: Manifest | null,
): { nodes: Node<AiudioNodeData>[]; edges: Edge[] } {
  const nodes = doc.nodes.map((n, i): Node<AiudioNodeData> => ({
    id: String(n.id),
    type: 'aiudio',
    position: n.position ? { x: n.position[0], y: n.position[1] } : { x: 60 + i * 200, y: 80 },
    data: {
      node: n.node,
      label: manifest?.kinds[n.node]?.type ?? n.node,
      params: n.params,
      manifest: manifest?.kinds[n.node] ?? null,
    },
  }))
  const edges = doc.edges.map((e): Edge => ({
    id: `${e.src}:${e.src_port}->${e.dst}:${e.dst_port}`,
    source: String(e.src),
    target: String(e.dst),
    sourceHandle: `out-${e.src_port}`,
    targetHandle: `in-${e.dst_port}`,
    data: { src: e.src, srcPort: e.src_port, dst: e.dst, dstPort: e.dst_port }, // for disconnect
  }))
  return { nodes, edges }
}

// Re-derive nodes/edges from a freshly-broadcast document while PRESERVING the local layout
// (drag positions) of nodes that already exist. New nodes take the document/fallback position;
// removed nodes drop out. Edges are always re-derived (they carry no layout). Pure + testable.
export function reconcile(
  current: Node<AiudioNodeData>[],
  doc: GraphDocument,
  manifest: Manifest | null,
): { nodes: Node<AiudioNodeData>[]; edges: Edge[] } {
  const positions = new Map(current.map((n) => [n.id, n.position]))
  const { nodes: fresh, edges } = documentToFlow(doc, manifest)
  const nodes = fresh.map((n) =>
    positions.has(n.id) ? { ...n, position: positions.get(n.id)! } : n,
  )
  return { nodes, edges }
}

/** Parse a React Flow handle id like ``"out-2"`` / ``"in-0"`` into its port number. */
export function handlePort(handleId: string | null | undefined): number {
  const n = Number(String(handleId ?? '').replace(/^\D+/, ''))
  return Number.isFinite(n) ? n : 0
}

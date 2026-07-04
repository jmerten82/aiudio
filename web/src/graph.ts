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
  }))
  return { nodes, edges }
}

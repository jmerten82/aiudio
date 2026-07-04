import { describe, expect, it } from 'vitest'

import { documentToFlow, handlePort, reconcile } from './graph'
import type { GraphDocument } from './types'

const doc: GraphDocument = {
  nodes: [
    { id: 0, node: 'source', args: {}, params: {}, position: null },
    { id: 1, node: 'gain', args: { gain: 0.5 }, params: { '0': 0.5 }, position: [10, 20] },
  ],
  edges: [{ src: 0, src_port: 0, dst: 1, dst_port: 0 }],
}

describe('documentToFlow', () => {
  it('maps nodes with ids, positions, and data', () => {
    const { nodes } = documentToFlow(doc, null)
    expect(nodes.map((n) => n.id)).toEqual(['0', '1'])
    expect(nodes[1].position).toEqual({ x: 10, y: 20 }) // saved layout preserved
    expect(nodes[0].position.x).toBeGreaterThanOrEqual(0) // fallback layout for no position
    expect(nodes[1].data.node).toBe('gain')
    expect(nodes[1].data.params).toEqual({ '0': 0.5 })
  })

  it('maps edges to handle-qualified React Flow edges', () => {
    const { edges } = documentToFlow(doc, null)
    expect(edges).toHaveLength(1)
    expect(edges[0]).toMatchObject({
      source: '0',
      target: '1',
      sourceHandle: 'out-0',
      targetHandle: 'in-0',
    })
  })

  it('labels a node from the manifest type when available', () => {
    const manifest = {
      kinds: { gain: { kind: 'gain', type: 'GainNode', num_inputs: 1, num_outputs: 1, params: [], config: {}, realtime_capable: true, defaults: {} } },
    }
    const { nodes } = documentToFlow(doc, manifest)
    expect(nodes[1].data.label).toBe('GainNode')
  })

  it('edges carry src/dst port metadata for disconnect', () => {
    const { edges } = documentToFlow(doc, null)
    expect(edges[0].data).toEqual({ src: 0, srcPort: 0, dst: 1, dstPort: 0 })
  })
})

describe('reconcile', () => {
  it('preserves local positions of existing nodes and adds new ones', () => {
    const first = documentToFlow(doc, null).nodes
    const moved = first.map((n) => (n.id === '1' ? { ...n, position: { x: 999, y: 999 } } : n))
    const doc2: GraphDocument = {
      nodes: [...doc.nodes, { id: 2, node: 'sink', args: {}, params: {}, position: null }],
      edges: doc.edges,
    }
    const { nodes } = reconcile(moved, doc2, null)
    expect(nodes.find((n) => n.id === '1')!.position).toEqual({ x: 999, y: 999 }) // kept
    expect(nodes.map((n) => n.id)).toEqual(['0', '1', '2'])                        // node 2 added
  })

  it('drops removed nodes', () => {
    const current = documentToFlow(doc, null).nodes
    const { nodes } = reconcile(current, { nodes: [doc.nodes[0]], edges: [] }, null)
    expect(nodes.map((n) => n.id)).toEqual(['0'])
  })
})

describe('handlePort', () => {
  it('parses the port number from a handle id', () => {
    expect(handlePort('out-2')).toBe(2)
    expect(handlePort('in-0')).toBe(0)
    expect(handlePort(null)).toBe(0)
  })
})

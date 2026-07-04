import { describe, expect, it } from 'vitest'

import { documentToFlow } from './graph'
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
      kinds: { gain: { kind: 'gain', type: 'GainNode', num_inputs: 1, num_outputs: 1, params: [], config: {}, realtime_capable: true } },
    }
    const { nodes } = documentToFlow(doc, manifest)
    expect(nodes[1].data.label).toBe('GainNode')
  })
})

import { describe, expect, it } from 'vitest'

import * as A from './actions'

// These builders must produce exactly the dicts the Python action space parses (ADR-0020) —
// op names + field names have to match aiudio.workbench.from_dict.
describe('action builders (wire contract)', () => {
  it('add_node carries node/args/position', () => {
    expect(A.addNode('gain', { gain: 0.5 }, [10, 20])).toEqual({
      type: 'action',
      action: { op: 'add_node', node: 'gain', args: { gain: 0.5 }, position: [10, 20] },
    })
    expect(A.addNode('source').action).toEqual({ op: 'add_node', node: 'source', args: {}, position: null })
  })

  it('connect / disconnect use src/src_port/dst/dst_port', () => {
    expect(A.connectNodes(0, 0, 1, 2).action).toEqual(
      { op: 'connect', src: 0, src_port: 0, dst: 1, dst_port: 2 })
    expect(A.disconnectNodes(0, 0, 1, 2).action).toEqual(
      { op: 'disconnect', src: 0, src_port: 0, dst: 1, dst_port: 2 })
  })

  it('set_param uses node/index/value; remove_node uses id', () => {
    expect(A.setParam(3, 1, 4.0).action).toEqual({ op: 'set_param', node: 3, index: 1, value: 4.0 })
    expect(A.removeNode(3).action).toEqual({ op: 'remove_node', id: 3 })
  })

  it('undo / redo are bare messages', () => {
    expect(A.undo()).toEqual({ type: 'undo' })
    expect(A.redo()).toEqual({ type: 'redo' })
  })
})

// Client → server messages. The `action` payloads mirror the Python action space (ADR-0020) exactly
// — op names + field names must match `aiudio.workbench.from_dict`. Kept as pure builders so the
// wire contract is unit-tested from the TS side (actions.test.ts).
import type { GraphDocument } from './types'

export interface ActionMessage { type: 'action'; action: Record<string, unknown> }
export type ClientMessage =
  | ActionMessage
  | { type: 'undo' }
  | { type: 'redo' }
  | { type: 'sync' }
  | { type: 'load'; doc: GraphDocument }

export const addNode = (
  node: string,
  args: Record<string, unknown> = {},
  position: [number, number] | null = null,
): ActionMessage => ({ type: 'action', action: { op: 'add_node', node, args, position } })

export const removeNode = (id: number): ActionMessage =>
  ({ type: 'action', action: { op: 'remove_node', id } })

export const connectNodes = (
  src: number, srcPort: number, dst: number, dstPort: number,
): ActionMessage =>
  ({ type: 'action', action: { op: 'connect', src, src_port: srcPort, dst, dst_port: dstPort } })

export const disconnectNodes = (
  src: number, srcPort: number, dst: number, dstPort: number,
): ActionMessage =>
  ({ type: 'action', action: { op: 'disconnect', src, src_port: srcPort, dst, dst_port: dstPort } })

export const setParam = (node: number, index: number, value: number): ActionMessage =>
  ({ type: 'action', action: { op: 'set_param', node, index, value } })

export const setPosition = (id: number, x: number, y: number): ActionMessage =>
  ({ type: 'action', action: { op: 'set_position', id, x, y } })

export const undo = (): ClientMessage => ({ type: 'undo' })
export const redo = (): ClientMessage => ({ type: 'redo' })
export const loadDocument = (doc: GraphDocument): ClientMessage => ({ type: 'load', doc })

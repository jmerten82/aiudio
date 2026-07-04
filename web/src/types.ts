// Wire types mirroring the aiudio server (A2) + workbench (A0/A1). The graph document and manifest
// come from the backend; the browser only renders them (B0 is read-only).

export interface ParamDescriptor {
  index: number
  name: string
  min: number
  max: number
  default: number
  unit: string
}

export interface NodeManifest {
  kind: string
  type: string
  num_inputs: number
  num_outputs: number
  params: ParamDescriptor[]
  config: Record<string, number>
  realtime_capable: boolean
  defaults: Record<string, unknown> // constructor args for "add this node"
}

export interface Manifest {
  kinds: Record<string, NodeManifest>
}

export interface GraphNode {
  id: number
  node: string
  args: Record<string, unknown>
  params: Record<string, number>
  position: [number, number] | null
}

export interface GraphEdge {
  src: number
  src_port: number
  dst: number
  dst_port: number
}

export interface GraphDocument {
  nodes: GraphNode[]
  edges: GraphEdge[]
}

export type ServerMessage =
  | { type: 'manifest'; manifest: Manifest }
  | { type: 'graph'; doc: GraphDocument }
  | { type: 'result'; value: unknown }
  | { type: 'error'; message: string }

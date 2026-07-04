import { Handle, Position, type NodeProps } from '@xyflow/react'

import type { AiudioNodeData } from './graph'

function fmt(v: number): string {
  return Number.isInteger(v) ? String(v) : v.toFixed(2)
}

// A custom React Flow node: title (node type) + a read-only list of its parameters (name = current
// value, from the manifest descriptors), with a handle per input/output port.
export function AiudioNode({ data }: NodeProps) {
  const d = data as AiudioNodeData
  const numIn = d.manifest?.num_inputs ?? 1
  const numOut = d.manifest?.num_outputs ?? 1
  const params = d.manifest?.params ?? []
  return (
    <div className="aiudio-node">
      {Array.from({ length: numIn }).map((_, i) => (
        <Handle key={`in-${i}`} id={`in-${i}`} type="target" position={Position.Left}
                style={{ top: 20 + i * 16 }} />
      ))}
      <div className="aiudio-node__title">{d.label}</div>
      {params.length > 0 && (
        <div className="aiudio-node__params">
          {params.map((p) => (
            <div key={p.index} className="aiudio-node__param">
              <span>{p.name}</span>
              <span>{fmt(d.params[String(p.index)] ?? p.default)}{p.unit ? ` ${p.unit}` : ''}</span>
            </div>
          ))}
        </div>
      )}
      {Array.from({ length: numOut }).map((_, i) => (
        <Handle key={`out-${i}`} id={`out-${i}`} type="source" position={Position.Right}
                style={{ top: 20 + i * 16 }} />
      ))}
    </div>
  )
}

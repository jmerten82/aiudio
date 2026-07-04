import type { GraphNode, NodeManifest } from './types'

// The selected node's editable parameters, driven by the manifest descriptors (name / range / unit).
// Each control sends a set_param action; the server validates + broadcasts, and the value re-renders
// from the authoritative document.
export function Inspector({ node, manifest, onSetParam, onRemove }: {
  node: GraphNode | null
  manifest: NodeManifest | null
  onSetParam: (index: number, value: number) => void
  onRemove: () => void
}) {
  if (!node) {
    return (
      <aside className="panel inspector">
        <h3>Inspector</h3>
        <p className="muted">Select a node to edit its parameters.</p>
      </aside>
    )
  }
  const params = manifest?.params ?? []
  return (
    <aside className="panel inspector">
      <h3>{manifest?.type ?? node.node} <span className="muted">#{node.id}</span></h3>
      {params.length === 0 && <p className="muted">No editable parameters.</p>}
      {params.map((p) => {
        const value = node.params[String(p.index)] ?? p.default
        const step = (p.max - p.min) / 100 || 0.01
        return (
          <label key={p.index} className="field">
            <span className="field__name">{p.name}{p.unit ? ` (${p.unit})` : ''}</span>
            <span className="field__row">
              <input type="range" min={p.min} max={p.max} step={step} value={value}
                     onChange={(e) => onSetParam(p.index, Number(e.target.value))} />
              <input type="number" className="field__num" min={p.min} max={p.max} value={value}
                     onChange={(e) => onSetParam(p.index, Number(e.target.value))} />
            </span>
          </label>
        )
      })}
      <button className="danger" onClick={onRemove}>Remove node</button>
    </aside>
  )
}

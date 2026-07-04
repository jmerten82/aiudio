import type { Manifest } from './types'

// The node palette — one button per manifest kind. Clicking adds that node (with the manifest's
// default constructor args) via an action.
export function Palette({ manifest, onAdd }: {
  manifest: Manifest | null
  onAdd: (kind: string) => void
}) {
  const kinds = manifest ? Object.keys(manifest.kinds).sort() : []
  return (
    <aside className="panel palette">
      <h3>Add node</h3>
      <div className="palette__list">
        {kinds.map((kind) => (
          <button key={kind} className="palette__item" title={manifest!.kinds[kind].type}
                  onClick={() => onAdd(kind)}>
            {kind}
          </button>
        ))}
      </div>
    </aside>
  )
}

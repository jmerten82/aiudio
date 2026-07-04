import { Background, Controls, ReactFlow } from '@xyflow/react'
import '@xyflow/react/dist/style.css'
import { useEffect, useMemo, useState } from 'react'

import { documentToFlow } from './graph'
import { AiudioNode } from './GraphView'
import type { GraphDocument, Manifest } from './types'
import { connect } from './ws'

const nodeTypes = { aiudio: AiudioNode }
const EMPTY: GraphDocument = { nodes: [], edges: [] }

export default function App() {
  const [manifest, setManifest] = useState<Manifest | null>(null)
  const [doc, setDoc] = useState<GraphDocument>(EMPTY)
  const [connected, setConnected] = useState(false)

  useEffect(() => {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws'
    const ws = connect(
      `${proto}://${location.host}/ws`,
      (m) => {
        if (m.type === 'manifest') setManifest(m.manifest)
        else if (m.type === 'graph') setDoc(m.doc)
      },
      setConnected,
    )
    return () => ws.close()
  }, [])

  const { nodes, edges } = useMemo(() => documentToFlow(doc, manifest), [doc, manifest])

  return (
    <div className="app">
      <header className="app__header">
        <strong>aiudio workbench</strong>
        <span className={`app__status app__status--${connected ? 'on' : 'off'}`}>
          {connected ? 'connected' : 'disconnected'}
        </span>
        <span className="app__count">{doc.nodes.length} nodes · {doc.edges.length} edges</span>
      </header>
      <div className="app__canvas">
        <ReactFlow nodes={nodes} edges={edges} nodeTypes={nodeTypes} fitView
                   nodesDraggable={false} nodesConnectable={false} elementsSelectable>
          <Background />
          <Controls showInteractive={false} />
        </ReactFlow>
      </div>
    </div>
  )
}

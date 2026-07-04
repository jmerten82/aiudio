import {
  Background,
  Controls,
  ReactFlow,
  useEdgesState,
  useNodesState,
  type Connection,
  type Edge,
  type Node,
} from '@xyflow/react'
import '@xyflow/react/dist/style.css'
import { useCallback, useEffect, useRef, useState } from 'react'

import * as A from './actions'
import { ChatPanel, type ChatMessage } from './ChatPanel'
import { AiudioNode } from './GraphView'
import { handlePort, reconcile, type AiudioNodeData } from './graph'
import { Inspector } from './Inspector'
import { Palette } from './Palette'
import type { GraphDocument, Manifest } from './types'
import { connect, send } from './ws'

const nodeTypes = { aiudio: AiudioNode }
const EMPTY: GraphDocument = { nodes: [], edges: [] }

export default function App() {
  const [manifest, setManifest] = useState<Manifest | null>(null)
  const [doc, setDoc] = useState<GraphDocument>(EMPTY)
  const [connected, setConnected] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [selected, setSelected] = useState<number | null>(null)
  const [chat, setChat] = useState<ChatMessage[]>([])
  const [agentBusy, setAgentBusy] = useState(false)
  const [nodes, setNodes, onNodesChange] = useNodesState<Node<AiudioNodeData>>([])
  const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>([])
  const wsRef = useRef<WebSocket | null>(null)

  // connect once; reconcile React Flow state from each server broadcast (preserving local layout)
  useEffect(() => {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws'
    const ws = connect(
      `${proto}://${location.host}/ws`,
      (m) => {
        if (m.type === 'manifest') setManifest(m.manifest)
        else if (m.type === 'graph') setDoc(m.doc)
        else if (m.type === 'error') { setError(m.message); setAgentBusy(false) }
        else if (m.type === 'agent') {
          setChat((c) => [...c, { role: 'agent', text: m.text, applied: m.applied.length }])
          setAgentBusy(false)
        }
      },
      setConnected,
    )
    wsRef.current = ws
    return () => ws.close()
  }, [])

  useEffect(() => {
    setNodes((prev) => reconcile(prev, doc, manifest).nodes)
    setEdges(reconcile(nodes, doc, manifest).edges)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [doc, manifest])

  const emit = useCallback((message: A.ClientMessage) => {
    setError(null)
    send(wsRef.current, message)
  }, [])

  // Debounce set_param so dragging a slider coalesces to ~one message per param per 40 ms.
  const paramTimers = useRef<Record<string, ReturnType<typeof setTimeout>>>({})
  const emitSetParam = useCallback((node: number, index: number, value: number) => {
    const key = `${node}:${index}`
    clearTimeout(paramTimers.current[key])
    paramTimers.current[key] = setTimeout(() => emit(A.setParam(node, index, value)), 40)
  }, [emit])

  const onNodeDragStop = useCallback((_e: unknown, node: Node) => {
    emit(A.setPosition(Number(node.id), node.position.x, node.position.y)) // persist layout
  }, [emit])

  const saveGraph = useCallback(() => {
    const blob = new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = 'aiudio-graph.json'
    a.click()
    URL.revokeObjectURL(url)
  }, [doc])

  const loadGraph = useCallback((file: File) => {
    file.text().then((text) => emit(A.loadDocument(JSON.parse(text)))).catch((e) => setError(String(e)))
  }, [emit])

  const sendChat = useCallback((text: string) => {
    setChat((c) => [...c, { role: 'user', text }])
    setAgentBusy(true)
    emit(A.chat(text))
  }, [emit])

  const onConnect = useCallback((c: Connection) => {
    emit(A.connectNodes(Number(c.source), handlePort(c.sourceHandle),
                        Number(c.target), handlePort(c.targetHandle)))
  }, [emit])

  const onNodesDelete = useCallback((deleted: Node[]) => {
    deleted.forEach((n) => emit(A.removeNode(Number(n.id))))
  }, [emit])

  const onEdgesDelete = useCallback((deleted: Edge[]) => {
    deleted.forEach((e) => {
      const d = e.data as { src: number; srcPort: number; dst: number; dstPort: number } | undefined
      if (d) emit(A.disconnectNodes(d.src, d.srcPort, d.dst, d.dstPort))
    })
  }, [emit])

  const addNode = useCallback((kind: string) => {
    const defaults = (manifest?.kinds[kind]?.defaults ?? {}) as Record<string, unknown>
    const pos: [number, number] = [120 + doc.nodes.length * 40, 120 + doc.nodes.length * 20]
    emit(A.addNode(kind, defaults, pos))
  }, [emit, manifest, doc.nodes.length])

  const selectedNode = doc.nodes.find((n) => n.id === selected) ?? null

  return (
    <div className="app">
      <header className="app__header">
        <strong>aiudio workbench</strong>
        <span className={`app__status app__status--${connected ? 'on' : 'off'}`}>
          {connected ? 'connected' : 'disconnected'}
        </span>
        <button onClick={() => emit(A.undo())}>Undo</button>
        <button onClick={() => emit(A.redo())}>Redo</button>
        <button onClick={saveGraph}>Save</button>
        <label className="app__load">Load
          <input type="file" accept="application/json" style={{ display: 'none' }}
                 onChange={(e) => e.target.files?.[0] && loadGraph(e.target.files[0])} />
        </label>
        <span className="app__count">{doc.nodes.length} nodes · {doc.edges.length} edges</span>
        {error && <span className="app__error" onClick={() => setError(null)}>⚠ {error}</span>}
      </header>
      <div className="app__body">
        <Palette manifest={manifest} onAdd={addNode} />
        <div className="app__canvas">
          <ReactFlow
            nodes={nodes} edges={edges} nodeTypes={nodeTypes}
            onNodesChange={onNodesChange} onEdgesChange={onEdgesChange}
            onConnect={onConnect} onNodesDelete={onNodesDelete} onEdgesDelete={onEdgesDelete}
            onNodeDragStop={onNodeDragStop}
            onSelectionChange={({ nodes: sel }) => setSelected(sel[0] ? Number(sel[0].id) : null)}
            fitView
          >
            <Background />
            <Controls />
          </ReactFlow>
        </div>
        <div className="app__right">
          <Inspector
            node={selectedNode}
            manifest={selectedNode ? manifest?.kinds[selectedNode.node] ?? null : null}
            onSetParam={(index, value) => selected !== null && emitSetParam(selected, index, value)}
            onRemove={() => selected !== null && emit(A.removeNode(selected))}
          />
          <ChatPanel messages={chat} busy={agentBusy} disabled={!connected} onSend={sendChat} />
        </div>
      </div>
    </div>
  )
}

import { useState } from 'react'

export interface ChatMessage {
  role: 'user' | 'agent'
  text: string
  applied?: number // how many graph edits the agent applied
}

// The agent companion (C1): type a request in natural language; the agent edits the graph over the
// same WS (the canvas updates from the broadcast). `busy` disables input while the agent is working.
export function ChatPanel({ messages, busy, disabled, onSend }: {
  messages: ChatMessage[]
  busy: boolean
  disabled: boolean
  onSend: (text: string) => void
}) {
  const [text, setText] = useState('')
  const submit = () => {
    const t = text.trim()
    if (t && !busy) {
      onSend(t)
      setText('')
    }
  }
  return (
    <div className="chat">
      <h3>Agent</h3>
      <div className="chat__log">
        {messages.length === 0 && (
          <p className="muted">Ask in plain language — e.g. "add a compressor after the source".</p>
        )}
        {messages.map((m, i) => (
          <div key={i} className={`chat__msg chat__msg--${m.role}`}>
            <div className="chat__text">{m.text}</div>
            {m.applied ? <div className="chat__applied">{m.applied} edit(s)</div> : null}
          </div>
        ))}
        {busy && <div className="chat__msg chat__msg--agent muted">…thinking</div>}
      </div>
      <div className="chat__input">
        <textarea
          rows={2}
          value={text}
          placeholder={disabled ? 'connecting…' : 'Describe a change…'}
          disabled={disabled || busy}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter' && !e.shiftKey) {
              e.preventDefault()
              submit()
            }
          }}
        />
        <button onClick={submit} disabled={disabled || busy || !text.trim()}>Send</button>
      </div>
    </div>
  )
}

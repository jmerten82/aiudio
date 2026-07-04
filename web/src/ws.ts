import type { ServerMessage } from './types'

// Minimal WebSocket client to the aiudio server (A2). B0 is read-only, so it only receives
// (manifest + graph broadcasts); sending actions arrives with B1.
export function connect(
  url: string,
  onMessage: (m: ServerMessage) => void,
  onStatus?: (connected: boolean) => void,
): WebSocket {
  const ws = new WebSocket(url)
  ws.onopen = () => onStatus?.(true)
  ws.onclose = () => onStatus?.(false)
  ws.onmessage = (event) => onMessage(JSON.parse(event.data) as ServerMessage)
  return ws
}

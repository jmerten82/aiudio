import react from '@vitejs/plugin-react'
import { defineConfig } from 'vitest/config'

// Dev: `npm run dev` serves the SPA and proxies API/WS to the localhost aiudio server (A2).
// Prod: `npm run build` emits web/dist, which `python -m aiudio.server --static web/dist` serves.
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': 'http://127.0.0.1:8765',
      '/ws': { target: 'ws://127.0.0.1:8765', ws: true },
    },
  },
  test: {
    environment: 'node', // the pure document→flow transform needs no DOM
    globals: true,
  },
})

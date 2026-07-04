import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// base './' so the production bundle loads over file:// in the Electron
// native (N-API) build. No server proxy: there is no backend to proxy to —
// all NetPulse data comes from window.netpulse over Electron IPC. Vite here
// only serves UI assets (HTML/JS/CSS) for hot-reload during development.
export default defineConfig({
  base: './',
  plugins: [react()],
})

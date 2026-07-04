import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// base './' so the production bundle loads over file:// in the Electron native
// (N-API) build. In dev, proxy /api to the C++ HTTP backend (no CORS friction).
export default defineConfig({
  base: './',
  plugins: [react()],
  server: { proxy: { '/api': 'http://127.0.0.1:8787' } },
})

import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import obfuscator from 'vite-plugin-bundle-obfuscator'
import process from 'node:process'

// Pattern verified against the official Tauri React template (tauri-apps/
// create-tauri-app) — fixed port 1420 (tauri.conf.json's devUrl expects it),
// ignore src-tauri so Rust rebuilds don't trigger a Vite reload loop, and the
// TAURI_DEV_HOST override for testing on a physical device/VM.
const host = process.env.TAURI_DEV_HOST

// Conservative options only — no debugProtection/selfDefending (both rely on
// eval-like Function-constructor tricks that tauri.conf.json's CSP
// (script-src 'self', no unsafe-eval) already blocks, so enabling them would
// just break the app at runtime for no benefit).
export default defineConfig(() => ({
  plugins: [
    react(),
    obfuscator({
      apply: 'build',
      enable: true,
      log: false,
      options: {
        compact: true,
        controlFlowFlattening: true,
        controlFlowFlatteningThreshold: 0.75,
        identifierNamesGenerator: 'hexadecimal',
        renameGlobals: false,
        selfDefending: false,
        debugProtection: false,
        disableConsoleOutput: false,
        stringArray: true,
        stringArrayThreshold: 0.75,
        splitStrings: true,
      },
    }),
  ],
  clearScreen: false,
  build: {
    sourcemap: false,
  },
  server: {
    port: 1420,
    strictPort: true,
    host: host || '127.0.0.1',
    hmr: host ? { protocol: 'ws', host, port: 1421 } : undefined,
    watch: { ignored: ['**/src-tauri/**'] },
  },
}))

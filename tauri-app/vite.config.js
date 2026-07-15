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
// eval-like Function-constructor tricks that could trip CSP depending on
// configuration, so leaving them off avoids that risk entirely).
//
// autoExcludeNodeModules is NOT optional here: without it, the obfuscator
// (control-flow flattening + identifier renaming) runs over React, ReactDOM,
// Recharts, D3, and @tauri-apps/api along with the app's own code, since
// Vite bundles everything into one chunk by default. Those libraries were
// never designed to survive obfuscation — this was confirmed to corrupt
// internal Map/Set-based logic, crashing every production build on launch
// with "Cannot read properties of undefined (reading 'has')" while `tauri
// dev` (which never runs this plugin — apply: 'build' only) worked fine.
// This splits node_modules into its own unobfuscated chunk; only the app's
// own src/ files get obfuscated, which was the actual point all along.
export default defineConfig(() => ({
  plugins: [
    react(),
    obfuscator({
      apply: 'build',
      enable: true,
      log: false,
      autoExcludeNodeModules: true,
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
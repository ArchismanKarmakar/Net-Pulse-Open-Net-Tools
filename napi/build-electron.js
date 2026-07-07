// Build the native addon against the LOCALLY INSTALLED Electron's ABI, so the
// Electron version never has to be hard-coded and can't drift out of sync when
// Electron is upgraded. Run `npm i` in ../electron first so Electron is present.
const { execSync } = require('child_process')
const path = require('path')

function electronVersion() {
  const candidates = [
    path.join(__dirname, '..', 'electron', 'node_modules', 'electron', 'package.json'),
    path.join(__dirname, 'node_modules', 'electron', 'package.json'),
  ]
  for (const p of candidates) {
    try { const v = require(p).version; if (v) return v } catch (_) { /* next */ }
  }
  return null
}

const v = electronVersion()
if (!v) {
  console.error('[build:electron] Electron not found. Run `npm i` in ../electron first, then retry.')
  process.exit(1)
}
console.log(`[build:electron] Building native addon for Electron ${v} ...`)
execSync(`npx cmake-js rebuild --runtime electron --runtime-version ${v}`, { stdio: 'inherit', cwd: __dirname })

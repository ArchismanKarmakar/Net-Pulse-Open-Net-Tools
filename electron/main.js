// NetPulse desktop app — pure native, no web server and no localhost port.
// The C++ engine runs in-process as a CMake.js Node-API addon (napi/) loaded
// here in the main process; the renderer talks to it only over IPC. This mirrors
// a Qt-style desktop app: no HTTP, no ws, no host:port.
const { app, BrowserWindow, dialog, shell, ipcMain, session } = require('electron')
const path = require('path')

const DEV = process.env.NETPULSE_DEV === '1'
app.setName('Net Pulse — Open Net Tools')
if (process.platform === 'win32') app.setAppUserModelId('com.archismankarmakar.netpulse.opennettools')

let win = null
let engine = null

function loadEngine() {
  const candidates = [
    path.join(__dirname, '..', 'napi'),
    path.join(process.resourcesPath || '', 'napi'),
    path.join(process.resourcesPath || '', 'app.asar.unpacked', 'napi'),
  ]
  for (const dir of candidates) {
    try {
      const mod = require(dir)
      if (mod) {
        const build = (mod.engineBuild || (mod.getEngineBuild && mod.getEngineBuild())) || 'unknown'
        console.log(`[Net Pulse] engine build: ${build}`)
        if (build === 'unknown') console.warn('[Net Pulse] engine has no build tag — this is an OLD addon; run build-and-run to recompile it.')
      }
      return mod
    } catch (_) { /* next */ }
  }
  return null
}

function distDir() {
  return app.isPackaged
    ? path.join(process.resourcesPath, 'web', 'dist')
    : path.join(__dirname, '..', 'web', 'dist')
}

function registerEngineIpc() {
  ipcMain.handle('np:getState', (_e, focus) => engine.getStateJSON(focus))
  ipcMain.handle('np:interfaces', () => engine.listInterfaces())
  ipcMain.handle('np:engineBuild', () => (engine && (engine.engineBuild || (engine.getEngineBuild && engine.getEngineBuild()))) || 'unknown')
  ipcMain.handle('np:add', (_e, opts) => engine.addTarget(opts))
  ipcMain.handle('np:update', (_e, id, opts) => engine.updateTarget(id, opts))
  ipcMain.handle('np:pause', (_e, id, on) => engine.pauseTarget(id, on))
  ipcMain.handle('np:stop', (_e, id) => engine.stopTarget(id))
  ipcMain.handle('np:remove', (_e, id) => engine.removeTarget(id))

  // ── Network tools (Node built-ins; no C++ engine needed) ──────────────────
  const dns = require('dns')
  const net = require('net')
  const { spawn } = require('child_process')

  const isHostish = (s) => typeof s === 'string' && /^[a-zA-Z0-9._:\-\[\]]{1,255}$/.test(s.trim())

  // Forward DNS: resolve a name to A + AAAA records.
  ipcMain.handle('np:tools:dns', async (_e, name) => {
    name = String(name || '').trim()
    if (!isHostish(name)) return { error: 'Invalid host name' }
    const out = { name, a: [], aaaa: [], cname: [], error: null }
    const tryResolve = (fn, key) => new Promise((res) => fn(name, (err, recs) => { if (!err && recs) out[key] = recs; res() }))
    try {
      await Promise.all([
        tryResolve(dns.resolve4, 'a'),
        tryResolve(dns.resolve6, 'aaaa'),
        tryResolve(dns.resolveCname, 'cname'),
      ])
      if (!out.a.length && !out.aaaa.length) {
        // fall back to a plain lookup (covers hosts files / single-record cases)
        await new Promise((res) => dns.lookup(name, { all: true }, (err, addrs) => {
          if (!err && addrs) for (const a of addrs) (a.family === 6 ? out.aaaa : out.a).push(a.address)
          res()
        }))
      }
    } catch (e) { out.error = String(e && e.message || e) }
    return out
  })

  // Reverse DNS: address → PTR names.
  ipcMain.handle('np:tools:reverse', async (_e, addr) => {
    addr = String(addr || '').trim()
    if (!isHostish(addr)) return { addr, names: [], error: 'Invalid address' }
    return new Promise((res) => dns.reverse(addr, (err, names) => res({ addr, names: names || [], error: err ? String(err.message) : null })))
  })

  // TCP port scan: connect-scan a range, bounded and rate-safe.
  ipcMain.handle('np:tools:portscan', async (_e, host, startPort, endPort) => {
    host = String(host || '').trim()
    let s = Math.max(1, Math.min(65535, parseInt(startPort, 10) || 1))
    let e = Math.max(1, Math.min(65535, parseInt(endPort, 10) || 1024))
    if (e < s) [s, e] = [e, s]
    if (e - s > 2048) e = s + 2048 // cap the range so a scan can't run unbounded
    if (!isHostish(host)) return { host, open: [], error: 'Invalid host' }
    const ports = []; for (let p = s; p <= e; p++) ports.push(p)
    const open = []
    const CONCURRENCY = 200, TIMEOUT = 800
    let idx = 0
    const worker = () => new Promise((resolveW) => {
      const next = () => {
        if (idx >= ports.length) return resolveW()
        const port = ports[idx++]
        const sock = new net.Socket()
        let done = false
        const finish = (isOpen) => { if (done) return; done = true; try { sock.destroy() } catch {} ; if (isOpen) open.push(port); next() }
        sock.setTimeout(TIMEOUT)
        sock.once('connect', () => finish(true))
        sock.once('timeout', () => finish(false))
        sock.once('error', () => finish(false))
        try { sock.connect(port, host) } catch { finish(false) }
      }
      next()
    })
    await Promise.all(Array.from({ length: Math.min(CONCURRENCY, ports.length) }, worker))
    open.sort((a, b) => a - b)
    return { host, scanned: [s, e], open, error: null }
  })

  // Ping a single host using the OS `ping` (authentic cmd-style output). Args
  // are passed as an array (never a shell string) so the validated host can't
  // inject. Streams lines back to the renderer over 'np:tools:ping:line'.
  const pingProcs = new Map()
  ipcMain.handle('np:tools:ping:start', (_e, host, opts) => {
    host = String(host || '').trim()
    if (!isHostish(host)) return { id: null, error: 'Invalid host' }
    opts = opts && typeof opts === 'object' ? opts : {}
    const isWin = process.platform === 'win32'
    const isMac = process.platform === 'darwin'
    const clampInt = (v, lo, hi, d) => { const n = parseInt(v, 10); return Number.isFinite(n) ? Math.max(lo, Math.min(hi, n)) : d }
    const count = clampInt(opts.count, 1, 10000, 10)
    const size = opts.size != null ? clampInt(opts.size, 0, 65500, 56) : null
    const ttl = opts.ttl != null ? clampInt(opts.ttl, 1, 255, null) : null
    const timeoutMs = opts.timeout != null ? clampInt(opts.timeout, 100, 60000, null) : null
    const interval = opts.interval != null ? Math.max(0.2, Math.min(60, Number(opts.interval) || 1)) : null
    const cont = !!opts.continuous
    const fam = opts.family === 'v4' ? '4' : opts.family === 'v6' ? '6' : null

    const args = []
    if (fam) args.push('-' + fam)
    if (isWin) {
      if (cont) args.push('-t'); else { args.push('-n', String(count)) }
      if (size != null) args.push('-l', String(size))
      if (ttl != null) args.push('-i', String(ttl))          // Windows: -i = TTL
      if (timeoutMs != null) args.push('-w', String(timeoutMs)) // Windows: -w = ms
    } else {
      if (!cont) args.push('-c', String(count))
      if (size != null) args.push('-s', String(size))
      if (ttl != null) args.push('-t', String(ttl))          // Unix: -t = TTL
      if (interval != null) args.push('-i', String(interval))
      if (timeoutMs != null) args.push(isMac ? '-t' : '-W', String(isMac ? Math.ceil(timeoutMs / 1000) : Math.ceil(timeoutMs / 1000)))
    }
    args.push(host)

    const id = Math.random().toString(36).slice(2)
    let proc
    try { proc = spawn('ping', args, { windowsHide: true }) } catch (e) { return { id: null, error: String(e.message) } }
    pingProcs.set(id, proc)
    const send = (chunk, stream) => { if (win && !win.isDestroyed()) win.webContents.send('np:tools:ping:line', { id, line: chunk.toString(), stream }) }
    proc.stdout.on('data', (d) => send(d, 'out'))
    proc.stderr.on('data', (d) => send(d, 'err'))
    proc.on('close', (code) => { pingProcs.delete(id); if (win && !win.isDestroyed()) win.webContents.send('np:tools:ping:done', { id, code }) })
    return { id, error: null, cmd: 'ping ' + args.join(' ') }
  })
  ipcMain.handle('np:tools:ping:stop', (_e, id) => { const p = pingProcs.get(id); if (p) { try { p.kill() } catch {} pingProcs.delete(id) } return true })
}

function applyContentSecurityPolicy() {
  // The renderer needs no privileged web permissions (camera, mic, geolocation,
  // notifications, USB, …). Deny every permission request/check outright.
  session.defaultSession.setPermissionRequestHandler((_wc, _perm, cb) => cb(false))
  if (session.defaultSession.setPermissionCheckHandler)
    session.defaultSession.setPermissionCheckHandler(() => false)
  // Allow self + https for the external BGP/routing lookups (RIPEstat, DoH, RDAP);
  // block everything else. No remote scripts, no eval.
  session.defaultSession.webRequest.onHeadersReceived((details, cb) => {
    cb({
      responseHeaders: {
        ...details.responseHeaders,
        'Content-Security-Policy': [
          "default-src 'self'; " +
          "script-src 'self'; " +
          "style-src 'self' 'unsafe-inline'; " +
          "img-src 'self' data:; " +
          "connect-src 'self' https:; " +
          "object-src 'none'; base-uri 'self'; frame-ancestors 'none'",
        ],
      },
    })
  })
}

async function createWindow() {
  win = new BrowserWindow({
    width: 1340, height: 880, backgroundColor: '#0e1116',
    title: 'Net Pulse — Open Net Tools',
    icon: path.join(__dirname, 'icon.ico'),
    webPreferences: {
      contextIsolation: true,   // renderer can't touch Node directly
      nodeIntegration: false,   // no Node in the renderer
      sandbox: true,            // renderer runs sandboxed
      webSecurity: true,        // enforce same-origin / CSP (explicit for audits)
      allowRunningInsecureContent: false,
      webviewTag: false,        // no <webview> embedding
      spellcheck: false,
      preload: path.join(__dirname, 'preload.js'),
    },
  })

  // The ONLY place this app ever touches a host:port. It is gated behind an
  // opt-in env var (default `npm start` never sets it), and even here the URL
  // serves UI ASSETS ONLY (HTML/JS/CSS from Vite's dev server, for hot-reload
  // while developing the React UI) — never NetPulse data. preload.js still
  // injects window.netpulse into this window exactly as it does for the
  // production file:// build, so all probing data flows over IPC either way.
  const url = DEV ? 'http://127.0.0.1:5173' : `file://${path.join(distDir(), 'index.html')}`
  win.loadURL(url)

  // External BGP / looking-glass links open in the OS browser; block in-app nav.
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:\/\//.test(url)) { shell.openExternal(url); return { action: 'deny' } }
    return { action: 'deny' }
  })
  win.webContents.on('will-navigate', (e, navUrl) => {
    if (navUrl !== url) e.preventDefault()
  })
  win.on('closed', () => { win = null })
}

app.whenReady().then(() => {
  engine = loadEngine()
  if (!engine) {
    dialog.showErrorBox('Net Pulse — Open Net Tools',
      'Native engine not found.\n\nBuild it first:\n  cd napi && npm install\n  npm run build:electron\n\n(Electron version ' +
      process.versions.electron + ')')
    app.quit()
    return
  }
  registerEngineIpc()
  applyContentSecurityPolicy()
  console.log('[NetPulse] in-process Node-API engine ready (no server, no port)')
  createWindow()
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow() })
})

app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit() })

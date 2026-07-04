// NetPulse desktop app — pure native, no web server and no localhost port.
// The C++ engine runs in-process as a CMake.js Node-API addon (napi/) loaded
// here in the main process; the renderer talks to it only over IPC. This mirrors
// a Qt-style desktop app: no HTTP, no ws, no host:port.
const { app, BrowserWindow, dialog, shell, ipcMain, session } = require('electron')
const path = require('path')

const DEV = process.env.NETPULSE_DEV === '1'
let win = null
let engine = null

function loadEngine() {
  const candidates = [
    path.join(__dirname, '..', 'napi'),
    path.join(process.resourcesPath || '', 'napi'),
    path.join(process.resourcesPath || '', 'app.asar.unpacked', 'napi'),
  ]
  for (const dir of candidates) {
    try { return require(dir) } catch (_) { /* next */ }
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
  ipcMain.handle('np:add', (_e, opts) => engine.addTarget(opts))
  ipcMain.handle('np:update', (_e, id, opts) => engine.updateTarget(id, opts))
  ipcMain.handle('np:pause', (_e, id, on) => engine.pauseTarget(id, on))
  ipcMain.handle('np:stop', (_e, id) => engine.stopTarget(id))
  ipcMain.handle('np:remove', (_e, id) => engine.removeTarget(id))
}

function applyContentSecurityPolicy() {
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
    title: 'NetPulse — Path Latency Studio',
    webPreferences: {
      contextIsolation: true,   // renderer can't touch Node directly
      nodeIntegration: false,   // no Node in the renderer
      sandbox: true,            // renderer runs sandboxed
      preload: path.join(__dirname, 'preload.js'),
    },
  })

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
    dialog.showErrorBox('NetPulse',
      'Native engine not found.\n\nBuild it first:\n  cd napi && npm install\n  npx cmake-js compile --runtime electron --runtime-version ' +
      process.versions.electron)
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

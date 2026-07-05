// Exposes the native engine to the renderer as window.netpulse. When this is
// present, the app uses it instead of HTTP fetch (see api() in App.jsx).
const { contextBridge, ipcRenderer } = require('electron')

contextBridge.exposeInMainWorld('netpulse', {
  getState: async (focus) => {
    const json = await ipcRenderer.invoke('np:getState', typeof focus === 'number' ? focus : undefined)
    return JSON.parse(json)
  },
  listInterfaces: () => ipcRenderer.invoke('np:interfaces'),
  addTarget: (opts) => ipcRenderer.invoke('np:add', opts),
  updateTarget: (id, opts) => ipcRenderer.invoke('np:update', id, opts),
  pauseTarget: (id, on) => ipcRenderer.invoke('np:pause', id, !!on),
  stopTarget: (id) => ipcRenderer.invoke('np:stop', id),
  removeTarget: (id) => ipcRenderer.invoke('np:remove', id),
  // Generic IPC helper used by UI tool components to call the main process
  _ipc: (channel, ...args) => ipcRenderer.invoke(channel, ...args),
  // Tools exposed directly for convenience
  tools: {
    dns: (name) => ipcRenderer.invoke('np:tools:dns', name),
    reverse: (addr) => ipcRenderer.invoke('np:tools:reverse', addr),
    portscan: (host, s, e) => ipcRenderer.invoke('np:tools:portscan', host, s, e),
    pingStart: (host, count) => ipcRenderer.invoke('np:tools:ping:start', host, count),
    pingStop: (id) => ipcRenderer.invoke('np:tools:ping:stop', id),
    onPingLine: (cb) => { const h = (_e, d) => cb(d); ipcRenderer.on('np:tools:ping:line', h); return () => ipcRenderer.removeListener('np:tools:ping:line', h) },
    onPingDone: (cb) => { const h = (_e, d) => cb(d); ipcRenderer.on('np:tools:ping:done', h); return () => ipcRenderer.removeListener('np:tools:ping:done', h) },
  },
  engineBuild: () => ipcRenderer.invoke('np:engineBuild'),
})

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
})

// Transport: ALL data flows through window.netpulse — the in-process C++
// engine bridged over Tauri commands by tauri-bridge.js. There is no HTTP
// fallback: this app never opens a socket, a port, or a WebSocket for its
// own data path. tauri-bridge.js installs window.netpulse before App.jsx's
// module code runs (see main.jsx), so a fallback was never actually
// reachable — removing it makes that guarantee explicit instead of implicit.
export const api = async (path, opts) => {
  const np = (typeof window !== 'undefined') && window.netpulse
  if (!np) throw new Error('Net Pulse native engine (window.netpulse) is not available — this UI must run inside the Net Pulse — Open Net Tools desktop app.')
  if (!path.startsWith('/api/')) throw new Error(`api(): unexpected path ${path}`)
  const [base, qs] = path.split('?')
  const q = {}; new URLSearchParams(qs || '').forEach((v, k) => { q[k] = v })
  switch (base) {
    case '/api/state': return np.getState(q.focus && q.focus !== 'all' ? Number(q.focus) : undefined)
    case '/api/interfaces': return np.listInterfaces()
    case '/api/add': return { id: await np.addTarget({ target: q.target, family: q.family, probe: Number(q.probe), trace: Number(q.trace), timeout: Number(q.timeout), payload: Number(q.payload), maxhops: Number(q.maxhops), raw: q.raw !== '0', src: q.src || '', protocol: q.protocol || 'icmp', destPort: q.destPort != null ? Number(q.destPort) : 33434 }) }
    case '/api/update': { const o = {}; ['probe', 'timeout', 'payload', 'maxhops', 'destPort'].forEach((k) => { if (q[k] != null) o[k] = Number(q[k]) }); if (q.family) o.family = q.family; if (q.src != null) o.src = q.src; if (q.protocol) o.protocol = q.protocol; if (q.pausedHops != null) o.pausedHops = q.pausedHops === '' ? [] : q.pausedHops.split(',').map(Number).filter((n) => Number.isFinite(n)); await np.updateTarget(Number(q.id), o); return { ok: true } }
    case '/api/pause': await np.pauseTarget(Number(q.id), q.on !== '0'); return {}
    case '/api/recheck': await np.forceRecheck(Number(q.id)); return {}
    case '/api/stop': await np.stopTarget(Number(q.id)); return {}
    case '/api/remove': await np.removeTarget(Number(q.id)); return {}
    default: throw new Error(`api(): unknown endpoint ${base}`)
  }
}

// Thin per-target control helper shared by the menu bar, sidebar strip, and
// dashboard table (pause/resume/recheck/remove all shape their query string
// the same way).
export const ctrl = (id, ep, extra = '') => api(`/api/${ep}?id=${id}${extra}`, { method: 'POST' })

// toolsApi() guards every standalone-tool page (Ping/DNS/Port Scanner)
// against running outside the Tauri app the same way api() does above.
export const toolsApi = () => (typeof window !== 'undefined' && window.netpulse && window.netpulse.tools) || null

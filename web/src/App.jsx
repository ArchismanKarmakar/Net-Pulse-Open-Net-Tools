import React, { useEffect, useMemo, useRef, useState } from 'react'
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, Legend,
} from 'recharts'
import * as bgp from './bgp'

const FOCUS = [
  ['Last 5 sec', 5], ['Last 10 sec', 10], ['Last 30 sec', 30],
  ['Last 1 min', 60], ['Last 2 min', 120], ['Last 5 min', 300], ['Last 10 min', 600],
  ['Last 30 min', 1800], ['Last 1 hour', 3600], ['Last 3 hours', 10800],
  ['Last 6 hours', 21600], ['Last 12 hours', 43200], ['Last 24 hours', 86400], ['All', 'all'],
]
const SCALE_STEPS = [25, 50, 75, 100, 150, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 5000]

// Transport shim: in the Electron native build, window.netpulse (the in-process
// C++ N-API engine) is present, so /api/* calls are dispatched to it directly —
// no localhost port, no HTTP. In the browser/dev build it falls back to fetch,
// so the same React code runs unchanged in both modes.
const api = async (path, opts) => {
  const np = (typeof window !== 'undefined') && window.netpulse
  if (np && path.startsWith('/api/')) {
    const [base, qs] = path.split('?')
    const q = {}; new URLSearchParams(qs || '').forEach((v, k) => { q[k] = v })
    switch (base) {
      case '/api/state': return np.getState(q.focus && q.focus !== 'all' ? Number(q.focus) : undefined)
      case '/api/interfaces': return np.listInterfaces()
      case '/api/add': return { id: await np.addTarget({ target: q.target, family: q.family, probe: Number(q.probe), trace: Number(q.trace), timeout: Number(q.timeout), payload: Number(q.payload), maxhops: Number(q.maxhops), raw: q.raw !== '0', src: q.src || '' }) }
      case '/api/update': { const o = {}; ['probe', 'timeout', 'payload', 'maxhops'].forEach((k) => { if (q[k] != null) o[k] = Number(q[k]) }); if (q.family) o.family = q.family; if (q.src != null) o.src = q.src; await np.updateTarget(Number(q.id), o); return { ok: true } }
      case '/api/pause': await np.pauseTarget(Number(q.id), q.on !== '0'); return {}
      case '/api/stop': await np.stopTarget(Number(q.id)); return {}
      case '/api/remove': await np.removeTarget(Number(q.id)); return {}
      default: break
    }
  }
  return fetch(path, opts).then((r) => r.json())
}
const fmt = (v) => (v === null || v === undefined ? '—' : Number(v).toFixed(1))
const timeFmt = (s) => new Date(s * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })

function hopColor(i, sel) {
  if (sel) return '#3a82f6'
  const h = (i * 0.61803398875) % 1
  return `hsl(${Math.round(h * 360)}, 60%, 60%)`
}
function latColor(ms, alertMs) {
  if (ms == null) return '#7a8699'
  const t = Math.max(0, Math.min(1, ms / Math.max(1, alertMs)))
  const r = t < 0.5 ? Math.round(t * 2 * 255) : 230
  const g = t < 0.5 ? 200 : Math.round(200 * (1 - (t - 0.5) * 2))
  return `rgb(${r},${g},60)`
}
function lossBg(loss) {
  return loss > 0 ? `rgba(248,81,73,${(0.10 + 0.24 * loss / 100).toFixed(3)})` : 'rgba(63,185,80,0.10)'
}
function niceCeil(v) {
  for (const s of SCALE_STEPS) if (v <= s) return s
  return Math.ceil(v / 1000) * 1000
}
function download(name, text, type = 'text/plain') {
  const blob = new Blob([text], { type })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a'); a.href = url; a.download = name; a.click()
  URL.revokeObjectURL(url)
}

const Sparkline = React.memo(function Sparkline({ points, color, w = 96, h = 22 }) {
  if (!points || points.length < 2) return <svg width={w} height={h} />
  const rtts = points.filter((p) => p[1] != null).map((p) => p[1])
  if (rtts.length < 2) return <svg width={w} height={h} />
  const lo = Math.min(...rtts), hi = Math.max(...rtts), span = hi - lo || 1, n = points.length
  const xy = (i, v) => [((i / (n - 1)) * (w - 2) + 1).toFixed(1), (h - 1 - ((v - lo) / span) * (h - 2)).toFixed(1)]
  const segs = []; let cur = []
  points.forEach((p, i) => { if (p[1] == null) { if (cur.length) { segs.push(cur); cur = [] } } else cur.push(xy(i, p[1]).join(',')) })
  if (cur.length) segs.push(cur)
  return (
    <svg width={w} height={h}>
      {points.map((p, i) => p[1] == null
        ? <line key={i} x1={(i / (n - 1)) * (w - 2) + 1} x2={(i / (n - 1)) * (w - 2) + 1} y1="0" y2={h} stroke="rgba(240,70,70,.3)" />
        : null)}
      {segs.map((s, i) => <polyline key={i} points={s.join(' ')} fill="none" stroke={color} strokeWidth="1.3" />)}
    </svg>
  )
})

// PingPlotter-style shared-scale latency graph: min–max whisker, avg circle,
// cur ✕, row shaded by loss, and a red line threading consecutive hops.
function LatencyGraph({ h, prevAvg, nextAvg, scaleMax, alertMs, w = 360, hgt = 24 }) {
  const X = (ms) => Math.max(0, Math.min(1, ms / scaleMax)) * (w - 10) + 5
  const yc = hgt / 2
  return (
    <svg width={w} height={hgt} className="latgraph">
      <rect x="0" y="0" width={w} height={hgt} fill={lossBg(h.loss)} />
      {h.avg != null && prevAvg != null && <line x1={X(prevAvg)} y1="0" x2={X(h.avg)} y2={yc} stroke="#e5484d" strokeWidth="1.4" />}
      {h.avg != null && nextAvg != null && <line x1={X(h.avg)} y1={yc} x2={X(nextAvg)} y2={hgt} stroke="#e5484d" strokeWidth="1.4" />}
      {h.min != null && h.max != null && (
        <g stroke="#9aa7b8">
          <line x1={X(h.min)} x2={X(h.max)} y1={yc} y2={yc} />
          <line x1={X(h.min)} x2={X(h.min)} y1={yc - 4} y2={yc + 4} />
          <line x1={X(h.max)} x2={X(h.max)} y1={yc - 4} y2={yc + 4} />
        </g>
      )}
      {h.avg != null && <circle cx={X(h.avg)} cy={yc} r="3.6" fill="#0e1116" stroke={latColor(h.avg, alertMs)} strokeWidth="1.8" />}
      {h.cur != null && <text x={X(h.cur)} y={yc + 4} fill={latColor(h.cur, alertMs)} fontSize="12" textAnchor="middle">✕</text>}
    </svg>
  )
}

function RpkiBadge({ status }) {
  if (!status) return null
  const m = { valid: ['#3fb950', 'RPKI valid'], invalid: ['#f85149', 'RPKI invalid'], 'unknown': ['#d29922', 'RPKI unknown'] }
  const [c, t] = m[status] || ['#7a8699', `RPKI ${status}`]
  return <span className="chip" style={{ borderColor: c, color: c }}>{t}</span>
}

function Drawer({ detail, onClose }) {
  if (!detail) return null
  const d = detail.data
  return (
    <div className="drawer">
      <div className="drawer-head">
        <b>{detail.ip}</b>
        <button className="x" onClick={onClose}>✕</button>
      </div>
      {detail.private ? <div className="muted">Private / non-routable address — no BGP data.</div>
        : detail.loading ? <div className="muted"><span className="spinner sm" /> Querying RIPEstat…</div>
        : !d ? <div className="muted">No data (offline, or not in the routing table).</div>
        : (
          <div className="drawer-body">
            <div className="kv"><span>ASN</span><b>{d.asn ? `AS${d.asn}` : '—'}</b></div>
            <div className="kv"><span>Network</span><b>{d.holder || d.netname || '—'}</b></div>
            <div className="kv"><span>Prefix</span><b>{d.prefix || '—'}</b></div>
            <div className="kv"><span>Routing</span><b>{d.announced === true ? 'announced' : d.announced === false ? 'not announced' : '—'}</b></div>
            <div className="kv"><span>RPKI</span><b><RpkiBadge status={d.rpki} /></b></div>
            <div className="kv"><span>Location</span><b>{[d.city, d.country].filter(Boolean).join(', ') || '—'}</b></div>
            {d.descr && <div className="kv"><span>Owner</span><b>{d.descr}</b></div>}
            {d.paths && d.paths.length > 0 && (
              <div className="paths">
                <div className="paths-h">Sample BGP AS-paths (RIS)</div>
                {d.paths.map((p, i) => <div key={i} className="aspath">{p}</div>)}
              </div>
            )}
            <div className="drawer-links">
              {d.asn && <a href={bgp.links.heAsn(d.asn)} target="_blank" rel="noreferrer">bgp.he.net AS{d.asn}</a>}
              <a href={bgp.links.heIp(detail.ip)} target="_blank" rel="noreferrer">he.net IP</a>
              {d.asn && <a href={bgp.links.peeringdb(d.asn)} target="_blank" rel="noreferrer">PeeringDB</a>}
              <a href={bgp.links.ripestat(detail.ip)} target="_blank" rel="noreferrer">RIPEstat</a>
            </div>
          </div>
        )}
    </div>
  )
}

export default function App() {
  const [form, setForm] = useState({ target: '', family: 'auto', probe: 1, trace: 30, timeout: 0, payload: 56, maxhops: 30, raw: true })
  const [focusIdx, setFocusIdx] = useState(6) // Last 10 min
  const [overlay, setOverlay] = useState(false)
  const [alerts, setAlerts] = useState({ on: true, ms: 150, loss: 5 })
  const [state, setState] = useState({ targets: [] })
  const [selId, setSelId] = useState(null)
  const [selHop, setSelHop] = useState(null)
  const [asn, setAsn] = useState({})
  const [detail, setDetail] = useState(null)
  const [iface, setIface] = useState('')
  const [interfaces, setInterfaces] = useState([])
  const [view, setView] = useState({ trend: true, latency: false })
  const [editing, setEditing] = useState(false)
  const [editForm, setEditForm] = useState(null)
  const [theme, setTheme] = useState(() => { try { return localStorage.getItem('np-theme') || 'dark' } catch { return 'dark' } })
  const chartRef = useRef(null)
  const focus = FOCUS[focusIdx][1]

  useEffect(() => {
    document.documentElement.dataset.theme = theme
    try { localStorage.setItem('np-theme', theme) } catch {}
  }, [theme])

  useEffect(() => { api('/api/interfaces').then((j) => setInterfaces(Array.isArray(j) ? j : [])).catch(() => {}) }, [])

  useEffect(() => {
    let alive = true
    const tick = async () => { try { const s = await api(`/api/state?focus=${focus}`); if (alive) setState(s) } catch {} }
    tick(); const h = setInterval(tick, 600) // faster refresh (payload is downsampled)
    return () => { alive = false; clearInterval(h) }
  }, [focus])

  const applyUpdate = async () => {
    if (!sel || !editForm) return
    const q = new URLSearchParams({ id: sel.id, probe: editForm.probe, timeout: editForm.timeout, payload: editForm.payload, maxhops: editForm.maxhops, family: editForm.family, src: editForm.src })
    try { await api(`/api/update?${q}`, { method: 'POST' }); setEditing(false) } catch {}
  }

  const targets = state.targets || []
  const sel = targets.find((t) => t.id === selId) || targets[0]

  const isAlerting = (h) => alerts.on && ((h.avg != null && h.avg > alerts.ms) || (h.sent > 0 && h.loss > alerts.loss))

  // Per-hop status dot (breakpoint-style). Intermediate hops that never answer
  // (100% loss) are GREY, not red — routers commonly deprioritise ICMP, so that
  // is informational, not a fault. Only real signals get warm colours.
  const hopStatus = (h) => {
    const noReply = h.sent > 0 && h.recv === 0
    const lat = h.med ?? h.avg
    const latHigh = lat != null && lat > alerts.ms
    const lossHigh = h.sent > 0 && h.loss > alerts.loss
    if (h.is_dest) {
      if (noReply) return 'down'                 // red — destination unreachable
      if (lossHigh && latHigh) return 'bad'      // orange — loss + high latency
      if (lossHigh || latHigh) return 'warn'     // amber — one problem
      if (h.loss > 0) return 'minor'             // light green — occasional loss
      return 'ok'                                // green
    }
    if (noReply) return 'silent'                 // grey — no ICMP reply (normal)
    if (lossHigh) return 'loss'                  // amber — real partial loss
    if (latHigh) return 'warn'                   // amber — elevated
    return 'ok'                                  // green
  }

  // Target health (drives the targets-list colour):
  //   green (ok) · light green (okloss, occasional loss) · yellow (warn, a middle
  //   hop is lossy/unreachable) · orange (bad, target loss + high latency) ·
  //   red (down, target unreachable)
  const targetState = (t) => {
    const hops = t.hops || []
    const d = hops.find((h) => h.is_dest) || (hops.length ? hops[hops.length - 1] : null)
    if (!d) return 'ok'
    const lat = d.med ?? d.avg
    const latHigh = lat != null && lat > alerts.ms
    const lossHigh = d.sent > 0 && d.loss > alerts.loss
    const noReply = d.sent > 0 && d.recv === 0
    const minorLoss = d.loss > 0 && !lossHigh
    const pathBad = hops.some((h) => !h.is_dest && h.sent > 0 && h.loss > alerts.loss)
    if (noReply) return 'down'               // red — target unreachable
    if (lossHigh || latHigh) return 'bad'    // orange — target unhealthy
    if (pathBad) return 'warn'               // yellow — upstream path trouble
    if (minorLoss) return 'okloss'           // light green — occasional loss
    return 'ok'
  }
  const STATE_LABEL = { ok: 'ok', okloss: 'minor loss', warn: 'path', bad: 'high latency/loss', down: 'unreachable' }

  useEffect(() => { if (sel && selHop === null) { const d = sel.hops.find((h) => h.is_dest); if (d) setSelHop(d.hop) } }, [sel, selHop])

  // ASN/network enrichment for public hop IPs (cached; only refetches on change)
  const ipKey = useMemo(() => sel ? [...new Set(sel.hops.map((h) => h.address).filter(bgp.isPublicIp))].sort().join(',') : '', [sel])
  useEffect(() => {
    if (!ipKey) return
    ipKey.split(',').filter(Boolean).forEach(async (ip) => {
      if (asn[ip]) return
      setAsn((a) => ({ ...a, [ip]: { loading: true } }))
      const info = await bgp.ipInfo(ip)
      setAsn((a) => ({ ...a, [ip]: { ...info, loading: false } }))
    })
  }, [ipKey]) // eslint-disable-line

  const addTarget = async () => {
    if (!form.target.trim()) return
    const q = new URLSearchParams({ target: form.target.trim(), family: form.family, probe: form.probe, trace: form.trace, timeout: form.timeout, payload: form.payload, maxhops: form.maxhops, raw: form.raw ? '1' : '0' })
    if (iface) q.set('src', iface)
    const r = await api(`/api/add?${q}`, { method: 'POST' })
    setForm({ ...form, target: '' }); if (r.id) { setSelId(r.id); setSelHop(null) }
  }
  const ctrl = (id, ep, extra = '') => fetch(`/api/${ep}?id=${id}${extra}`, { method: 'POST' })
  const openDetail = (ip) => {
    if (!bgp.isPublicIp(ip)) { setDetail({ ip, private: true, loading: false }); return }
    setDetail({ ip, loading: true })
    bgp.hopDetails(ip).then((data) => setDetail((cur) => (cur && cur.ip === ip ? { ip, loading: false, data } : cur)))
  }

  const shownHops = useMemo(() => (sel ? sel.hops.filter((h) => overlay || h.hop === selHop || h.is_dest) : []), [sel, overlay, selHop])
  const scaleMax = useMemo(() => {
    if (!sel) return 100
    const mx = Math.max(0, ...sel.hops.map((h) => h.max ?? 0).filter((v) => Number.isFinite(v)))
    return niceCeil(Math.max(25, mx))
  }, [sel])

  const chartData = useMemo(() => {
    if (!sel) return []
    const byTs = new Map()
    for (const h of shownHops) for (const [ts, rtt] of sel.series[String(h.hop)] || []) {
      if (!byTs.has(ts)) byTs.set(ts, { ts }); byTs.get(ts)[`h${h.hop}`] = rtt
    }
    return [...byTs.values()].sort((a, b) => a.ts - b.ts)
  }, [sel, shownHops])

  const exportPng = () => {
    const svg = chartRef.current?.querySelector('svg'); if (!svg) return
    const xml = new XMLSerializer().serializeToString(svg); const img = new Image()
    img.onload = () => {
      const c = document.createElement('canvas'); c.width = svg.clientWidth * 2; c.height = svg.clientHeight * 2
      const ctx = c.getContext('2d'); ctx.fillStyle = '#0e1116'; ctx.fillRect(0, 0, c.width, c.height); ctx.scale(2, 2); ctx.drawImage(img, 0, 0)
      const a = document.createElement('a'); a.href = c.toDataURL('image/png'); a.download = 'netpulse_graph.png'; a.click()
    }
    img.src = 'data:image/svg+xml;base64,' + btoa(unescape(encodeURIComponent(xml)))
  }
  const exportHopsCsv = () => {
    if (!sel) return
    const head = 'hop,address,hostname,asn,network,loss_pct,cur,avg,min,max,jitter,std,sent,recv,is_dest,alerting\n'
    const rows = sel.hops.map((h) => {
      const a = asn[h.address] || {}
      return [h.hop, h.address, h.hostname, a.asn ? `AS${a.asn}` : '', (a.holder || '').replace(/,/g, ' '), h.loss.toFixed(2), h.cur ?? '', h.avg ?? '', h.min ?? '', h.max ?? '', (h.jitter ?? 0).toFixed(2), h.std.toFixed(2), h.sent, h.recv, h.is_dest, isAlerting(h)].join(',')
    }).join('\n')
    download('netpulse_hops.csv', `# ${sel.name} ${sel.dest_ip}\n` + head + rows, 'text/csv')
  }
  const exportTargets = (json) => {
    if (json) download('netpulse_targets.json', JSON.stringify(targets.map((t) => ({ id: t.id, target: t.name, dest_ip: t.dest_ip })), null, 2), 'application/json')
    else download('netpulse_targets.csv', 'id,target,dest_ip\n' + targets.map((t) => `${t.id},${t.name},${t.dest_ip}`).join('\n'), 'text/csv')
  }

  const dest = sel ? sel.hops.find((h) => h.is_dest) : null
  const asnOf = (ip) => asn[ip] || null

  return (
    <div className="app">
      <header>
        <h1>NetPulse <span>— Path Latency Studio</span>
          <button className="themebtn" title="Toggle light / dark" onClick={() => setTheme((t) => t === 'light' ? 'dark' : 'light')}>{theme === 'light' ? '🌙' : '☀'}</button>
        </h1>
        <div className="controls">
          <input className="target" placeholder="host or IP (8.8.8.8, 2001:4860:4860::8888, example.com)"
            value={form.target} onChange={(e) => setForm({ ...form, target: e.target.value })}
            onKeyDown={(e) => e.key === 'Enter' && addTarget()} />
          <button className="primary" onClick={addTarget}>＋ Add</button>
          <label>Family<select value={form.family} onChange={(e) => setForm({ ...form, family: e.target.value })}>
            <option value="auto">Auto</option><option value="v4">IPv4</option><option value="v6">IPv6</option></select></label>
          <label>Probe<input type="number" step="0.1" value={form.probe} onChange={(e) => setForm({ ...form, probe: e.target.value })} /></label>
          <label>Trace<input type="number" value={form.trace} onChange={(e) => setForm({ ...form, trace: e.target.value })} /></label>
          <label>Timeout<input type="number" step="0.1" value={form.timeout} onChange={(e) => setForm({ ...form, timeout: e.target.value })} /></label>
          <label>Payload<input type="number" value={form.payload} onChange={(e) => setForm({ ...form, payload: e.target.value })} /></label>
          <label>Hops<input type="number" value={form.maxhops} onChange={(e) => setForm({ ...form, maxhops: e.target.value })} /></label>
          <label className="cb"><input type="checkbox" checked={form.raw} onChange={(e) => setForm({ ...form, raw: e.target.checked })} />Raw</label>
          <label>Interface
            <select value={iface} onChange={(e) => setIface(e.target.value)}>
              <option value="">Auto (default route)</option>
              {interfaces
                .filter((i) => form.family === 'auto' || (form.family === 'v6') === i.v6)
                .map((i, k) => <option key={k} value={i.address}>{i.name} — {i.address}</option>)}
            </select>
          </label>
        </div>
        <div className="controls second">
          <label>Focus<select value={focusIdx} onChange={(e) => setFocusIdx(+e.target.value)}>
            {FOCUS.map(([l], i) => <option key={i} value={i}>{l}</option>)}</select></label>
          <label className="cb"><input type="checkbox" checked={overlay} onChange={(e) => setOverlay(e.target.checked)} />Overlay all hops</label>
          <span className="divider" />
          <label className="cb"><input type="checkbox" checked={alerts.on} onChange={(e) => setAlerts({ ...alerts, on: e.target.checked })} />Alerts</label>
          <label>&gt;<input type="number" disabled={!alerts.on} value={alerts.ms} onChange={(e) => setAlerts({ ...alerts, ms: +e.target.value })} />ms</label>
          <label>&gt;<input type="number" disabled={!alerts.on} value={alerts.loss} onChange={(e) => setAlerts({ ...alerts, loss: +e.target.value })} />% loss</label>
          <span className="divider" />
          <span className="legend"><i className="g" />0<i className="ramp" />{alerts.ms}ms+</span>
          <span className="divider" />
          <label className="cb"><input type="checkbox" checked={view.trend} onChange={(e) => setView({ ...view, trend: e.target.checked })} />Trend</label>
          <label className="cb"><input type="checkbox" checked={view.latency} onChange={(e) => setView({ ...view, latency: e.target.checked })} />Latency graph</label>
        </div>
      </header>

      <div className="body">
        <aside>
          <h3>Targets</h3>
          <ul>
            {targets.map((t) => {
              const st = targetState(t)
              return (
                <li key={t.id} className={(sel && t.id === sel.id ? 'sel ' : '') + 's-' + st} onClick={() => { setSelId(t.id); setSelHop(null) }}>
                  <span className={'dot st-' + st} /> {t.name}
                  {STATE_LABEL[st] && <span className={'statelabel st-' + st}>{STATE_LABEL[st]}</span>}
                  <small>{t.dest_ip} {t.family}{t.paused ? ' · paused' : ''}</small>
                </li>
              )
            })}
          </ul>
          {sel && (
            <div className="row">
              <button onClick={() => ctrl(sel.id, 'pause', `&on=${sel.paused ? 0 : 1}`)}>{sel.paused ? '▶ Resume' : '⏸ Pause'}</button>
              <button onClick={() => ctrl(sel.id, 'stop')}>⏹ Stop</button>
              <button onClick={() => { ctrl(sel.id, 'remove'); setSelId(null) }}>🗑</button>
            </div>
          )}
          <button onClick={() => exportTargets(false)}>Export targets CSV</button>
          <button onClick={() => exportTargets(true)}>Export targets JSON</button>
        </aside>

        <main>
          {!sel ? <div className="empty">Add a target to begin.</div> : (
            <>
              <div className="statusbar">
                <b>{sel.name}</b> → {sel.dest_ip || 'unresolved'} <span className="badge">{sel.family}</span> · {sel.hops.length} hops
                {(() => { const p = sel.hops.filter((h) => ['loss', 'warn', 'bad', 'down'].includes(hopStatus(h))); return p.length > 0 && <span className="err"> ⚠ {p.length} hop{p.length > 1 ? 's' : ''} with loss/latency</span> })()}
                {sel.error && <span className="err"> ⚠ {sel.error}</span>}
                <div className="spacer" />
                {dest && <span className="rtt">RTT <b>{fmt(dest.med ?? dest.avg)}</b> ms <span className="rttsub">(median · avg {fmt(dest.avg)} · cur {dest.cur == null ? '*' : fmt(dest.cur)})</span></span>}
                <button onClick={exportPng}>🖼 Graph PNG</button>
                <button onClick={exportHopsCsv}>📄 Hops CSV</button>
              </div>

              {sel.config && (
                <div className="cfgstrip">
                  <span className="cfglabel">CONFIG</span>
                  <span>probe <b>{sel.config.probe}s</b></span>
                  <span>timeout <b>{sel.config.timeout === 0 ? 'auto' : sel.config.timeout + 's'}</b></span>
                  <span>payload <b>{sel.config.payload}B</b></span>
                  <span>max hops <b>{sel.config.maxhops}</b></span>
                  <span>family <b>{sel.config.family}</b></span>
                  <span>iface <b>{sel.config.src || 'auto'}</b></span>
                  <span>mode <b>{sel.config.raw ? 'raw' : 'dgram'}</b></span>
                  <div className="spacer" />
                  {!editing
                    ? <button onClick={() => { setEditForm({ probe: sel.config.probe, timeout: sel.config.timeout, payload: sel.config.payload, maxhops: sel.config.maxhops, family: sel.config.family, src: sel.config.src }); setEditing(true) }}>⚙ Edit config</button>
                    : <button onClick={() => setEditing(false)}>✕ Close</button>}
                </div>
              )}

              {editing && editForm && (
                <div className="editpanel">
                  <label>Probe (s)<input type="number" step="0.1" min="0.1" value={editForm.probe} onChange={(e) => setEditForm({ ...editForm, probe: e.target.value })} /></label>
                  <label>Timeout (s, 0=auto)<input type="number" step="0.1" min="0" value={editForm.timeout} onChange={(e) => setEditForm({ ...editForm, timeout: e.target.value })} /></label>
                  <label>Payload<input type="number" value={editForm.payload} onChange={(e) => setEditForm({ ...editForm, payload: e.target.value })} /></label>
                  <label>Max hops<input type="number" value={editForm.maxhops} onChange={(e) => setEditForm({ ...editForm, maxhops: e.target.value })} /></label>
                  <label>Family
                    <select value={editForm.family} onChange={(e) => setEditForm({ ...editForm, family: e.target.value })}>
                      <option value="auto">Auto</option><option value="v4">IPv4</option><option value="v6">IPv6</option>
                    </select>
                  </label>
                  <label>Interface
                    <select value={editForm.src} onChange={(e) => setEditForm({ ...editForm, src: e.target.value })}>
                      <option value="">Auto (default route)</option>
                      {interfaces.filter((i) => editForm.family === 'auto' || (editForm.family === 'v6') === i.v6).map((i, k) => <option key={k} value={i.address}>{i.name} — {i.address}</option>)}
                    </select>
                  </label>
                  <button className="apply" onClick={applyUpdate}>Apply (live)</button>
                </div>
              )}

              {sel.hops.length === 0 && !sel.error ? (
                <div className="loading">
                  <div className="spinner" />
                  <div className="loading-text">{sel.dest_ip ? `Tracing route to ${sel.dest_ip}…` : `Resolving ${sel.name}…`}</div>
                  <div className="loading-sub">first samples appear in a couple of seconds</div>
                </div>
              ) : (
                <>
                  <div className="tablewrap">
                    <table>
                      <thead><tr>
                        <th>Hop</th><th>PL%</th><th>IP</th><th>Host</th><th>ASN</th><th>Network</th>
                        <th>Sent</th><th>Recv</th><th>Loss%</th><th>Cur</th><th>Avg</th><th>Min</th><th>Max</th><th>Jitter</th>
                        {view.trend && <th>Trend</th>}
                        {view.latency && <th className="lathead"><span>Latency Graph</span><span className="scalemax">{scaleMax} ms</span></th>}
                      </tr></thead>
                      <tbody>
                        {sel.hops.map((h, i) => {
                          const a = asnOf(h.address)
                          const prev = i > 0 ? asnOf(sel.hops[i - 1].address) : null
                          const boundary = a && prev && a.asn && prev.asn && a.asn !== prev.asn
                          const prevAvg = i > 0 ? sel.hops[i - 1].avg : null
                          const nextAvg = i < sel.hops.length - 1 ? sel.hops[i + 1].avg : null
                          return (
                            <tr key={h.hop} className={h.hop === selHop ? 'selrow' : ''} onClick={() => setSelHop(h.hop)}>
                              <td><span className={'hopdot st-' + hopStatus(h)} title={hopStatus(h)} />{h.hop}{h.is_dest ? ' ◀' : ''}</td>
                              <td><div className="plbar"><span style={{ width: `${Math.min(100, h.loss)}%` }} /></div><i className={h.loss > 0 ? 'loss' : ''}>{h.loss.toFixed(0)}</i></td>
                              <td className="mono">{h.address}</td>
                              <td className="host">{h.hostname}</td>
                              <td className={'asn' + (boundary ? ' boundary' : '')} onClick={(e) => { e.stopPropagation(); openDetail(h.address) }}>
                                {a ? (a.loading ? '…' : a.asn ? `AS${a.asn}` : '—') : (bgp.isPublicIp(h.address) ? '…' : 'priv')}
                              </td>
                              <td className="net" title={a && a.holder} onClick={(e) => { e.stopPropagation(); openDetail(h.address) }}>{a && a.holder ? a.holder : ''}</td>
                              <td>{h.sent}</td>
                              <td className={h.recv < h.sent ? 'loss' : ''}>{h.recv}</td>
                              <td className={h.loss > 0 ? 'loss' : ''}>{h.loss.toFixed(1)}%</td>
                              <td style={{ color: latColor(h.cur, alerts.ms), fontWeight: 600 }}>{h.cur == null ? '*' : fmt(h.cur)}</td>
                              <td>{fmt(h.avg)}</td><td>{fmt(h.min)}</td><td>{fmt(h.max)}</td><td>{fmt(h.jitter)}</td>
                              {view.trend && <td><Sparkline points={sel.series[String(h.hop)] || []} color={hopColor(i, h.hop === selHop)} /></td>}
                              {view.latency && <td className="latcell"><LatencyGraph h={h} prevAvg={prevAvg} nextAvg={nextAvg} scaleMax={scaleMax} alertMs={alerts.ms} /></td>}
                            </tr>
                          )
                        })}
                      </tbody>
                    </table>
                  </div>

                  <div className="chart" ref={chartRef}>
                    <ResponsiveContainer width="100%" height="100%">
                      <LineChart data={chartData} margin={{ top: 10, right: 20, bottom: 4, left: 0 }}>
                        <CartesianGrid stroke="var(--grid)" />
                        <XAxis dataKey="ts" type="number" domain={["dataMin","dataMax"]} tickFormatter={timeFmt} stroke="var(--axis)" tick={{ fill: "var(--axis)" }} fontSize={12} />
                        <YAxis stroke="var(--axis)" tick={{ fill: "var(--axis)" }} fontSize={12} label={{ value: "ms", angle: -90, position: "insideLeft", fill: "var(--axis)" }} />
                        <Tooltip labelFormatter={timeFmt} contentStyle={{ background: "var(--panel)", border: "1px solid var(--border)", borderRadius: 6, color: "var(--text)" }} labelStyle={{ color: "var(--muted)" }} itemStyle={{ color: "var(--text)" }} />
                        <Legend />
                        {shownHops.map((h, i) => (
                          <Line key={h.hop} type="linear" dataKey={`h${h.hop}`} name={`hop ${h.hop}`} stroke={hopColor(i, h.hop === selHop)} dot={false} connectNulls={false} strokeWidth={h.hop === selHop ? 2.5 : 1.3} isAnimationActive={false} />
                        ))}
                      </LineChart>
                    </ResponsiveContainer>
                  </div>
                </>
              )}
            </>
          )}
        </main>

        <Drawer detail={detail} onClose={() => setDetail(null)} />
      </div>
    </div>
  )
}

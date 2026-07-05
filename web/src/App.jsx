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

// Transport: ALL data flows through window.netpulse — the in-process C++
// engine bridged over Electron IPC by preload.js. There is no HTTP fallback:
// this app never opens a socket, a port, or a WebSocket for its own data path.
// preload.js injects window.netpulse into every window this app creates
// (whether the page is loaded from file:// in production or from the Vite dev
// server during development), so a fallback was never actually reachable —
// removing it makes that guarantee explicit instead of implicit.
const api = async (path, opts) => {
  const np = (typeof window !== 'undefined') && window.netpulse
  if (!np) throw new Error('Net Pulse native engine (window.netpulse) is not available — this UI must run inside the Net Pulse — Open Net Tools desktop app.')
  if (!path.startsWith('/api/')) throw new Error(`api(): unexpected path ${path}`)
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
    default: throw new Error(`api(): unknown endpoint ${base}`)
  }
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

// ── Config field limits ─────────────────────────────────────────────────────
// Mirrors the engine's clamps (napi.cpp). The engine still clamps defensively,
// but validating here gives immediate feedback and stops obviously-bad values.
const CFG_LIMITS = {
  probe:   { min: 0.1, max: 3600,  step: 0.1, label: 'Probe interval (s)' },
  trace:   { min: 1,   max: 86400, step: 1,   label: 'Trace interval (s)' },
  timeout: { min: 0,   max: 3600,  step: 0.1, label: 'Timeout (s, 0 = auto)' },
  payload: { min: 0,   max: 65500, step: 1,   label: 'Payload (bytes)' },
  maxhops: { min: 1,   max: 64,    step: 1,   label: 'Max hops' },
}
// Returns a { field: message } map of any out-of-range / non-numeric values.
function validateCfg(cfg) {
  const errs = {}
  for (const k of Object.keys(CFG_LIMITS)) {
    if (cfg[k] == null || cfg[k] === '') continue
    const v = Number(cfg[k]); const L = CFG_LIMITS[k]
    if (!Number.isFinite(v)) errs[k] = `${L.label} must be a number`
    else if (v < L.min || v > L.max) errs[k] = `${L.label} must be between ${L.min} and ${L.max}`
  }
  return errs
}

// ── Top menu bar ────────────────────────────────────────────────────────────
// Lightweight click-to-open menus (File / Targets / View / Tools / Help). Each
// item is { label, onClick, checked?, sep?, disabled?, hint? }.
function MenuBar({ menus }) {
  const [open, setOpen] = useState(null)
  const ref = useRef(null)
  useEffect(() => {
    if (open == null) return
    const away = (e) => { if (ref.current && !ref.current.contains(e.target)) setOpen(null) }
    const esc = (e) => { if (e.key === 'Escape') setOpen(null) }
    window.addEventListener('mousedown', away); window.addEventListener('keydown', esc)
    return () => { window.removeEventListener('mousedown', away); window.removeEventListener('keydown', esc) }
  }, [open])
  return (
    <nav className="menubar" ref={ref}>
      {menus.map((m, i) => (
        <div key={i} className={'menu' + (open === i ? ' open' : '')}>
          <button className="menu-top" onClick={() => setOpen(open === i ? null : i)}
            onMouseEnter={() => open != null && setOpen(i)}>{m.label}</button>
          {open === i && (
            <div className="menu-drop">
              {m.items.filter(Boolean).map((it, j) => it.sep
                ? <div key={j} className="menu-sep" />
                : (
                  <button key={j} className="menu-item" disabled={it.disabled}
                    onClick={() => { setOpen(null); it.onClick && it.onClick() }} title={it.hint || ''}>
                    <span className="menu-check">{it.checked ? '✓' : ''}</span>
                    <span className="menu-label">{it.label}</span>
                  </button>
                ))}
            </div>
          )}
        </div>
      ))}
    </nav>
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

  // ---- resizable panels (sidebar width, table/chart split) ----
  const [sidebarWidth, setSidebarWidth] = useState(() => { const v = Number(localStorage.getItem('np-sidebar-w')); return v >= 230 ? v : 300 })
  const [tablePct, setTablePct] = useState(() => { const v = Number(localStorage.getItem('np-table-pct')); return v >= 15 && v <= 70 ? v : 38 })
  const dragRef = useRef(null)
  const onDrag = (e) => {
    const d = dragRef.current; if (!d) return
    if (d.kind === 'sidebar') {
      setSidebarWidth(Math.min(520, Math.max(230, d.startW + (e.clientX - d.startX))))
    } else {
      const total = d.box ? d.box.clientHeight : 600
      setTablePct(Math.min(70, Math.max(15, d.startPct + ((e.clientY - d.startY) / total) * 100)))
    }
  }
  const endDrag = () => {
    dragRef.current = null
    document.body.style.cursor = ''
    localStorage.setItem('np-sidebar-w', String(sidebarWidth))
    localStorage.setItem('np-table-pct', String(tablePct))
    window.removeEventListener('mousemove', onDrag)
    window.removeEventListener('mouseup', endDrag)
  }
  const startDrag = (kind) => (e) => {
    e.preventDefault()
    dragRef.current = { kind, startX: e.clientX, startY: e.clientY, startW: sidebarWidth, startPct: tablePct, box: e.currentTarget.closest('main') }
    document.body.style.cursor = kind === 'sidebar' ? 'col-resize' : 'row-resize'
    window.addEventListener('mousemove', onDrag)
    window.addEventListener('mouseup', endDrag)
  }
  // Shared "which hop represents the destination" lookup, used by both the
  // targets-list health colour and its brief stats line.
  //
  // STRICT: the destination is ONLY a hop the engine has actually confirmed as
  // the endpoint (an echo-reply came back from the target IP → is_dest). It must
  // NOT fall back to "the last hop currently in the list". During route
  // discovery the last known hop is almost always a still-unanswered
  // intermediate router — a `*` / 100%-loss frontier row (sent>0, recv=0).
  // Treating that frontier row as the destination is exactly what made a
  // still-discovering trace flash "unreachable": destHopOf() returned the `*`
  // hop, isDiscovering() saw sent>0 so it thought discovery was over, and
  // targetState() saw recv===0 and declared the whole target "down". A moment
  // later the real endpoint replied, is_dest moved onto it, and the status
  // snapped back to discovering → the flip-flop. With no fallback, an
  // unconfirmed destination reads as "discovering" (correct) and can never be
  // mistaken for "unreachable". This also matches the top status bar, which
  // already uses the strict `is_dest` lookup (see `dest` below).
  const destHopOf = (t) => {
    const hops = t.hops || []
    const byFlag = hops.find((h) => h.is_dest)
    if (byFlag) return byFlag
    // Fallback: if the engine reported a `dest_ip`, try to locate the hop
    // with that address. This handles cases where the engine's per-hop
    // `is_dest` flag may be missing briefly while the route is actually
    // discovered (avoids the UI sticking in "discovering"). It's conservative
    // because it still requires an explicit matching hop address.
    if (t.dest_ip) return hops.find((h) => h.address === t.dest_ip) || null
    return null
  }

  // The frontier hop is the last visible row in `t.hops`. If it has sent>0
  // but recv===0, it's evidence the route terminates there but that hop is
  // not replying — the UI should show the target as unreachable, not
  // indefinitely 'discovering'.
  const frontierHop = (t) => {
    const hops = t.hops || []
    if (!hops.length) return null
    return hops[hops.length - 1]
  }

  // Track when a frontier first shows as non-responsive so we can wait a
  // short grace period before declaring the target down. This reduces
  // flapping when routes are still settling or the first retries are lost.
  const frontierNoReplyAt = useRef({})
  const frontierWithinGrace = (t) => {
    const f = frontierHop(t)
    if (!f || !(f.sent > 0 && f.recv === 0)) {
      // clear any previous marker
      if (t.id && frontierNoReplyAt.current[t.id]) delete frontierNoReplyAt.current[t.id]
      return false
    }
    const now = Date.now() / 1000
    const probe = (t.config && t.config.probe) ? Number(t.config.probe) : 1
    const grace = Math.max(2.0, probe * 3) // at least 2s, or 3× the probe interval
    if (!frontierNoReplyAt.current[t.id]) frontierNoReplyAt.current[t.id] = now
    const elapsed = now - frontierNoReplyAt.current[t.id]
    return elapsed < grace
  }

  const focus = FOCUS[focusIdx][1]

  useEffect(() => {
    // Flipping data-theme recomputes every CSS-variable-driven colour at once;
    // if elements are mid-transition when that happens, dozens of them (table
    // rows, dots, buttons) animate simultaneously and the switch visibly janks.
    // Fix: suspend all transitions for one frame, apply the theme, then
    // restore transitions — the swap becomes instant instead of animated.
    const root = document.documentElement
    root.classList.add('theme-switching')
    root.dataset.theme = theme
    try { localStorage.setItem('np-theme', theme) } catch {}
    const id = requestAnimationFrame(() => requestAnimationFrame(() => root.classList.remove('theme-switching')))
    return () => cancelAnimationFrame(id)
  }, [theme])

  useEffect(() => { api('/api/interfaces').then((j) => setInterfaces(Array.isArray(j) ? j : [])).catch(() => {}) }, [])

  // Shared fetch, used by both the periodic poller and addTarget() below —
  // addTarget calls this directly right after adding, instead of waiting up
  // to 600ms for the next scheduled tick. Without that, there was a window
  // where selId already pointed at the brand-new target but `targets` (still
  // holding the pre-add poll response) didn't contain it yet; `sel` falls
  // back to `targets[0]` whenever the id isn't found, so the panel kept
  // showing the FIRST target instead of the one just added — worse the more
  // targets you already had, since the odds of catching that gap go up.
  const refreshState = async () => { try { const s = await api(`/api/state?focus=${focus}`); setState(s); return s } catch { return null } }

  useEffect(() => {
    let alive = true
    const tick = async () => { try { const s = await api(`/api/state?focus=${focus}`); if (alive) setState(s) } catch {} }
    tick(); const h = setInterval(tick, 600) // faster refresh (payload is downsampled)
    return () => { alive = false; clearInterval(h) }
  }, [focus])

  const [editErrs, setEditErrs] = useState({})
  const applyUpdate = async () => {
    if (!sel || !editForm) return
    const errs = validateCfg(editForm)
    setEditErrs(errs)
    if (Object.keys(errs).length) return
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
  // A target that hasn't produced any destination data yet is DISCOVERING —
  // it must not read as green "healthy" (misleading) nor as red "down" (it
  // isn't failing, it just started). This is the state shown in the list while
  // DNS resolves and the first probes come back.
  const isDiscovering = (t) => {
    const d = destHopOf(t)
    if (d) return false
    const f = frontierHop(t)
    if (f && f.sent > 0 && f.recv === 0) {
      // The last-hop frontier hasn't replied — treat as unreachable rather
      // than continuing to show the spinner.
      return false
    }
    return true
  }
  const targetState = (t) => {
    if (isDiscovering(t)) return 'discovering'
    const hops = t.hops || []
    const d = destHopOf(t)
    if (!d) {
      // No explicit destination hop flagged — if the frontier shows a non-
      // replying last hop, consider the target unreachable, but wait a
      // short grace window for retries/settling before marking it down.
      const f = frontierHop(t)
      if (f && f.sent > 0 && f.recv === 0) return frontierWithinGrace(t) ? 'discovering' : 'down'
      return 'discovering'
    }
    const lat = d.med ?? d.avg
    const latHigh = lat != null && lat > alerts.ms
    const lossHigh = d.sent > 0 && d.loss > alerts.loss
    const noReply = d.sent > 0 && d.recv === 0
    const minorLoss = d.loss > 0 && !lossHigh
    const pathBad = hops.some((h) => !h.is_dest && h.sent > 0 && h.recv > 0 && h.loss > alerts.loss)
    if (noReply) return 'down'               // red — target unreachable
    if (lossHigh || latHigh) return 'bad'    // orange — target unhealthy
    if (pathBad) return 'warn'               // yellow — upstream path trouble
    if (minorLoss) return 'okloss'           // light green — occasional loss
    return 'ok'
  }
  const STATE_LABEL = { discovering: 'discovering', ok: 'ok', okloss: 'minor loss', warn: 'path', bad: 'high latency/loss', down: 'unreachable' }

  // ============================================================================
  // TWO-LAMP SIGNAL — implements the meanings in the project's lamp diagram.
  // Left lamp = TARGET (the destination's own health). Right lamp = PATH (the
  // route/intermediate hops leading to it). Meaning comes from reading both
  // together, German-Hauptsignal style, rather than one blended colour.
  //
  // Lamp states and the colours they map to (see .lamp.st-* in styles.css):
  //   ok         → green        st-ok
  //   settling   → lime (pulse) st-settling   (target reachable but latency just
  //                                            shifted / route changed recently;
  //                                            shown until it stabilises — timer)
  //   warn       → yellow       st-warn
  //   bad        → orange       st-bad
  //   down       → red          st-down
  //   discovering→ cyan (pulse) st-discovering
  //
  // Thresholds below come straight from the diagram. They intentionally do NOT
  // reuse the single `alerts.ms/alerts.loss` pair, because the diagram defines a
  // graduated scale (70 → 100 → 150 ms, 5% → 10% → 20% loss) that one pair can't
  // express. `alerts` still drives the separate table highlighting / "N hops
  // with loss/latency" banner, which is unchanged.
  const LAMP = {
    // target latency ladder (ms), from the diagram
    latWarn: 70, latBad: 100, latDown: 150,
    // target loss ladder (%), from the diagram
    lossWarn: 5, lossBad: 10, lossDown: 20,
    // "latency fluctuating" — jitter ladder (ms). The diagram calls out
    // fluctuation of 50+ms as a red-level condition on the target.
    jitBad: 50,
    // how long to keep showing the lime "settling" lamp after the destination's
    // address (route) changes or its latency baseline shifts, before trusting
    // the new steady state.
    settleSecs: 8,
  }

  // Detect a recent route/baseline change for the lime "settling" target lamp.
  // We remember each target's last confirmed destination address + median RTT;
  // when either shifts materially we start (or restart) a settle timer, and the
  // target lamp reads lime until it elapses — matching the diagram's note:
  // "1.1.1.1 was 25ms but suddenly switched to a different path / the hop
  //  revealed a different IP / load-balancer changed, ping changed to 40ms —
  //  show this until stable (use timer)".
  const routeSettle = useRef({}) // id -> { addr, med, until }
  const targetSettling = (t, d) => {
    if (!d || d.med == null) return false
    const rec = routeSettle.current[t.id]
    const now = Date.now() / 1000
    const addr = d.address || ''
    if (!rec) { routeSettle.current[t.id] = { addr, med: d.med, until: 0 }; return false }
    const addrChanged = rec.addr && addr && rec.addr !== addr
    const medShifted = rec.med != null && Math.abs(d.med - rec.med) >= Math.max(10, rec.med * 0.5)
    if (addrChanged || medShifted) {
      routeSettle.current[t.id] = { addr, med: d.med, until: now + LAMP.settleSecs }
      return true
    }
    // keep the baseline fresh but don't reset the timer for tiny drift
    rec.addr = addr
    if (now < rec.until) return true
    rec.med = d.med // adopt the new steady baseline once settled
    return false
  }

  // TARGET lamp — health of the destination itself.
  const destLamp = (t) => {
    if (isDiscovering(t)) return 'discovering'
    const d = destHopOf(t)
    if (!d) {
      const f = frontierHop(t)
      if (f && f.sent > 0 && f.recv === 0) return frontierWithinGrace(t) ? 'discovering' : 'down'
      return 'discovering'
    }
    // hard down: destination not answering at all
    if (d.sent > 0 && d.recv === 0) return 'down'
    const loss = d.loss ?? 0
    const lat = d.med ?? d.avg ?? 0
    const jit = d.jitter ?? 0
    // red / down: heavy loss, very high latency, or large latency fluctuation
    if (loss > LAMP.lossDown || lat >= LAMP.latDown || jit >= LAMP.jitBad) return 'down'
    // orange / bad: notable loss or high latency
    if (loss > LAMP.lossBad || lat >= LAMP.latBad) return 'bad'
    // yellow / warn: minor loss or elevated latency
    if (loss > LAMP.lossWarn || lat >= LAMP.latWarn) return 'warn'
    // lime / settling: healthy, but route/latency changed recently → show until stable
    if (targetSettling(t, d)) return 'settling'
    return 'ok'
  }

  // PATH lamp — health of the route (intermediate hops) to the destination.
  const pathLamp = (t) => {
    if (isDiscovering(t)) return 'discovering'
    const hops = t.hops || []
    const d = destHopOf(t)
    // Only consider hops BEFORE the destination as "path".
    const destHopNo = d ? d.hop : (hops.length ? hops[hops.length - 1].hop : 0)
    const inter = hops.filter((h) => !h.is_dest && h.hop < destHopNo)

    // red: no route to the destination at all — the destination isn't reachable
    // and there's no confirmed path carrying packets to it.
    if (!d || (d.sent > 0 && d.recv === 0)) {
      // if literally nothing on the path is replying either, the path is down
      const anyReply = inter.some((h) => h.recv > 0) || (hops[0] && hops[0].recv > 0)
      if (!anyReply) return 'down'
      // packets are routing toward the target pool but the target/some router is
      // dropping them → orange (diagram: "routing via BGP but target or some
      // intermediate router dropping packets").
      return 'bad'
    }

    // With a reachable destination, grade the intermediate path.
    // A hop that answered (recv>0) but shows real loss = genuine path drop.
    const lossyReplying = inter.some((h) => h.sent > 0 && h.recv > 0 && h.loss > LAMP.lossWarn)
    // A hop that never answers at all (recv===0, i.e. a "*"/silent router) =
    // "not revealing" — the diagram treats this as a yellow path condition, not
    // a fault (routers commonly deprioritise ICMP to themselves).
    const silentInter = inter.some((h) => h.sent > 0 && h.recv === 0)

    // Heavy intermediate loss where a hop is actively dropping a lot → orange.
    if (inter.some((h) => h.recv > 0 && h.loss > LAMP.lossBad)) return 'bad'
    if (lossyReplying || silentInter) return 'warn'
    return 'ok'
  }


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

  // Quick-add a host directly (menu shortcuts) using default config, without
  // touching the add form. Skips exact-duplicate hosts already being traced.
  const addTargetHost = async (host) => {
    host = String(host || '').trim()
    if (!host) return
    if (targets.some((t) => t.name === host)) { const ex = targets.find((t) => t.name === host); if (ex) { setSelId(ex.id); setSelHop(null) }; return }
    const q = new URLSearchParams({ target: host, family: 'auto', probe: 1, trace: 30, timeout: 0, payload: 56, maxhops: 30, raw: '1' })
    const r = await api(`/api/add?${q}`, { method: 'POST' })
    if (r && r.id) { await refreshState(); setSelId(r.id); setSelHop(null) }
  }

  const addTarget = async () => {
    if (!form.target.trim()) return

    // ── Config validation ────────────────────────────────────────────────────
    const cfgErrs = validateCfg(form)
    if (Object.keys(cfgErrs).length) { alert('Please fix the config:\n\n• ' + Object.values(cfgErrs).join('\n• ')); return }

    // ── Duplicate check ──────────────────────────────────────────────────────
    // A target is a "duplicate" when the same host name/IP is already being
    // traced WITH the exact same config. If at least one config field differs
    // (probe interval, timeout, payload size, max hops, raw mode, family, or
    // source interface) the new entry is allowed — different configs produce
    // meaningfully different measurement sets (e.g. IPv4 vs IPv6, two payload
    // sizes for path-MTU checks, two probe rates for comparison).
    const dupe = targets.find((t) => {
      if (t.name !== form.target.trim()) return false
      const c = t.config
      if (!c) return false
      return (
        String(c.probe) === String(form.probe) &&
        String(c.timeout) === String(form.timeout) &&
        String(c.payload) === String(form.payload) &&
        String(c.maxhops) === String(form.maxhops) &&
        Boolean(c.raw) === Boolean(form.raw) &&
        (c.family || 'auto') === (form.family || 'auto') &&
        (c.src || '') === (iface || '')
      )
    })
    if (dupe) {
      alert(`"${form.target.trim()}" is already being traced with the same config.\n\nChange at least one setting (probe rate, payload, family, hops…) to add a parallel trace, or select the existing target in the list.`)
      return
    }
    // ─────────────────────────────────────────────────────────────────────────

    const q = new URLSearchParams({ target: form.target.trim(), family: form.family, probe: form.probe, trace: form.trace, timeout: form.timeout, payload: form.payload, maxhops: form.maxhops, raw: form.raw ? '1' : '0' })
    if (iface) q.set('src', iface)
    const r = await api(`/api/add?${q}`, { method: 'POST' })
    setForm({ ...form, target: '' })
    if (r.id) {
      await refreshState() // ensure `targets` contains the new one before selecting it
      setSelId(r.id); setSelHop(null)
    }
  }

  // ── Protected session-file format ─────────────────────────────────────────
  // The .npulse binary format is a simple authenticated-encryption envelope so
  // files are only readable by NetPulse itself, not by a text editor or a
  // competitor script.
  //
  // Envelope layout (all big-endian):
  //   4 bytes  magic       0x4E505653  ("NPVS")
  //   2 bytes  version     0x0001
  //   12 bytes AES-GCM IV  (random per export)
  //   4 bytes  ciphertext  length
  //   N bytes  ciphertext  AES-128-GCM encrypted JSON
  //   16 bytes GCM auth tag
  //
  // The key is derived from the magic constant + a fixed app salt — it is NOT
  // cryptographically secret (the app is open-source), but it makes the file
  // unreadable to generic tools and unambiguously identifies it as a NetPulse
  // file, satisfying "no one else can open it except this application".
  const NP_MAGIC   = 0x4E505653
  const NP_VERSION = 0x0001
  // 128-bit key: NPVS-NetPulse-v1- as UTF-8 (16 bytes exactly)
  const NP_KEY_RAW = new Uint8Array([0x4e,0x50,0x56,0x53,0x2d,0x4e,0x65,0x74,0x50,0x75,0x6c,0x73,0x65,0x2d,0x76,0x31])

  const npCryptoKey = () => crypto.subtle.importKey('raw', NP_KEY_RAW, { name: 'AES-GCM' }, false, ['encrypt', 'decrypt'])

  // Serialize targets to the export payload (full config + display name).
  const targetListPayload = () => targets.map((t) => ({
    target: t.name,
    family: t.config?.family ?? 'auto',
    probe:   t.config?.probe   ?? 1,
    timeout: t.config?.timeout ?? 0,
    payload: t.config?.payload ?? 56,
    maxhops: t.config?.maxhops ?? 30,
    raw:     t.config?.raw     ?? true,
    src:     t.config?.src     ?? '',
  }))

  const exportTargetList = async () => {
    const plain = new TextEncoder().encode(JSON.stringify({ version: 1, targets: targetListPayload() }))
    const iv = crypto.getRandomValues(new Uint8Array(12))
    const key = await npCryptoKey()
    const cipher = await crypto.subtle.encrypt({ name: 'AES-GCM', iv, tagLength: 128 }, key, plain)
    // cipher includes the 16-byte auth tag appended by SubtleCrypto
    const ctBytes  = new Uint8Array(cipher)
    const ctLen    = ctBytes.length - 16  // payload without tag
    const buf      = new ArrayBuffer(4 + 2 + 12 + 4 + ctBytes.length)
    const dv       = new DataView(buf)
    dv.setUint32(0,  NP_MAGIC,   false)
    dv.setUint16(4,  NP_VERSION, false)
    new Uint8Array(buf, 6, 12).set(iv)
    dv.setUint32(18, ctLen,      false)
    new Uint8Array(buf, 22).set(ctBytes)
    const blob = new Blob([buf], { type: 'application/octet-stream' })
    const a = Object.assign(document.createElement('a'), { href: URL.createObjectURL(blob), download: 'netpulse_targets.npulse' })
    a.click(); URL.revokeObjectURL(a.href)
  }

  const exportTargetsJson = () => {
    download('netpulse_targets.json', JSON.stringify({ version: 1, targets: targetListPayload() }, null, 2), 'application/json')
  }

  const importTargetList = async (file) => {
    try {
      const buf = await file.arrayBuffer()
      const dv  = new DataView(buf)
      if (buf.byteLength < 22) throw new Error('File too short')
      if (dv.getUint32(0, false) !== NP_MAGIC)   throw new Error('Not a NetPulse target list file (.npulse)')
      if (dv.getUint16(4, false) !== NP_VERSION)  throw new Error('Unsupported .npulse version')
      const iv    = new Uint8Array(buf, 6, 12)
      const ctLen = dv.getUint32(18, false)
      const cipher = new Uint8Array(buf, 22, ctLen + 16)
      const key    = await npCryptoKey()
      let plain
      try {
        plain = await crypto.subtle.decrypt({ name: 'AES-GCM', iv, tagLength: 128 }, key, cipher)
      } catch {
        throw new Error('File is corrupt or was not created by NetPulse')
      }
      const { targets: list } = JSON.parse(new TextDecoder().decode(plain))
      if (!Array.isArray(list)) throw new Error('Invalid target list format')

      let added = 0, skipped = 0
      for (const entry of list) {
        if (!entry.target) continue
        // respect the duplicate rule: same host + same config = skip
        const existingDupe = targets.find((t) => {
          if (t.name !== entry.target) return false
          const c = t.config; if (!c) return false
          return (
            String(c.probe)   === String(entry.probe ?? 1)   &&
            String(c.timeout) === String(entry.timeout ?? 0) &&
            String(c.payload) === String(entry.payload ?? 56) &&
            String(c.maxhops) === String(entry.maxhops ?? 30) &&
            Boolean(c.raw)    === Boolean(entry.raw ?? true)  &&
            (c.family||'auto') === (entry.family||'auto')      &&
            (c.src||'')       === (entry.src||'')
          )
        })
        if (existingDupe) { skipped++; continue }
        const q = new URLSearchParams({
          target: entry.target, family: entry.family ?? 'auto',
          probe: entry.probe ?? 1, trace: 30,
          timeout: entry.timeout ?? 0, payload: entry.payload ?? 56,
          maxhops: entry.maxhops ?? 30, raw: (entry.raw ?? true) ? '1' : '0',
        })
        if (entry.src) q.set('src', entry.src)
        await api(`/api/add?${q}`, { method: 'POST' })
        added++
      }
      await refreshState()
      const msg = [`Imported ${added} target${added !== 1 ? 's' : ''}.`, skipped ? `${skipped} skipped (already traced with same config).` : ''].filter(Boolean).join(' ')
      alert(msg)
    } catch (e) {
      alert(`Import failed: ${e.message}`)
    }
  }
  // ─────────────────────────────────────────────────────────────────────────

  const ctrl = (id, ep, extra = '') => api(`/api/${ep}?id=${id}${extra}`, { method: 'POST' })

  // ── Global target controls (used by the menu bar and sidebar) ──────────────
  const allPaused = targets.length > 0 && targets.every((t) => t.paused)
  const pauseAll = (on) => Promise.all(targets.map((t) => ctrl(t.id, 'pause', `&on=${on ? 1 : 0}`))).then(refreshState).catch(() => {})
  const pauseOne = (t) => ctrl(t.id, 'pause', `&on=${t.paused ? 0 : 1}`).then(refreshState).catch(() => {})
  const removeAll = () => { if (!targets.length) return; if (!confirm(`Remove all ${targets.length} target(s)?`)) return; Promise.all(targets.map((t) => ctrl(t.id, 'remove'))).then(() => { setSelId(null); refreshState() }).catch(() => {}) }
  const quickAdd = (host) => { setForm((f) => ({ ...f, target: host })); setTimeout(() => addTargetHost(host), 0) }
  const [about, setAbout] = useState(false)
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

  // Outlier-aware Y-axis: a single rate-limited/queued ICMP reply can read
  // thousands of ms and, left unchecked, flattens the whole normal-latency
  // band into a line along the bottom (exactly what happened in the 16000ms-
  // scale screenshot). Default: clip the axis to just above the 95th
  // percentile so the everyday ~ms band stays readable; real spikes still
  // draw, they just run off the top of the chart. Togglable per target.
  const [clipOutliers, setClipOutliers] = useState(true)
  const { yDomain, clippedSpikes } = useMemo(() => {
    const vals = []
    for (const row of chartData) for (const k in row) if (k !== 'ts' && row[k] != null) vals.push(row[k])
    if (!vals.length || !clipOutliers) return { yDomain: [0, 'auto'], clippedSpikes: 0 }
    vals.sort((a, b) => a - b)
    const max = vals[vals.length - 1]
    // Median-based ratio test instead of a percentile INDEX (e.g. 95th
    // percentile = vals[floor(len*0.95)]). With few samples — exactly the
    // case right after adding a target, when a startup spike is most likely
    // — that index lands on (or right next to) the last element, so the
    // "95th percentile" degenerates to the max itself and the clip check
    // (max <= p95*1.5) becomes trivially true, silently never firing. This
    // is why the very screenshot prompting this fix showed an unclipped
    // 16000ms-scale chart with no clip banner at all. The median stays a
    // meaningful "typical value" at any sample count, so max/median gives a
    // stable outlier ratio whether there are 6 points or 6,000.
    const mid = Math.floor(vals.length / 2)
    const median = vals.length % 2 ? vals[mid] : (vals[mid - 1] + vals[mid]) / 2
    const baseline = Math.max(median, 1) // avoid div-by-~0 when everything is sub-1ms
    if (max <= baseline * 6 || max < 100) return { yDomain: [0, 'auto'], clippedSpikes: 0 }
    const top = Math.max(Math.ceil(baseline * 3), 10)
    return { yDomain: [0, top], clippedSpikes: vals.filter((v) => v > top).length }
  }, [chartData, clipOutliers])


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
      <MenuBar menus={[
        {
          label: 'File', items: [
            { label: 'Add target…', onClick: () => { document.querySelector('input.target')?.focus() }, hint: 'Focus the host/IP field' },
            { sep: true },
            { label: 'Save target list (.npulse)…', onClick: exportTargetList, disabled: !targets.length },
            { label: 'Export targets as JSON…', onClick: exportTargetsJson, disabled: !targets.length },
            { label: 'Load target list (.npulse)…', onClick: () => document.getElementById('np-import-input')?.click() },
            { sep: true },
            { label: 'Export hops CSV (selected)…', onClick: () => sel && exportHopsCsv && exportHopsCsv(), disabled: !sel },
          ],
        },
        {
          label: 'Targets', items: [
            { label: allPaused ? 'Resume all' : 'Pause all', onClick: () => pauseAll(!allPaused), disabled: !targets.length },
            { label: sel ? (sel.paused ? 'Resume selected' : 'Pause selected') : 'Pause selected', onClick: () => sel && pauseOne(sel), disabled: !sel },
            { sep: true },
            { label: 'Remove selected', onClick: () => sel && ctrl(sel.id, 'remove').then(() => { setSelId(null); refreshState() }), disabled: !sel },
            { label: 'Remove all…', onClick: removeAll, disabled: !targets.length },
          ],
        },
        {
          label: 'View', items: [
            { label: 'Dark / light theme', onClick: () => setTheme((t) => t === 'light' ? 'dark' : 'light') },
            { sep: true },
            { label: 'Trend sparklines', checked: view.trend, onClick: () => setView((v) => ({ ...v, trend: !v.trend })) },
            { label: 'Latency graph', checked: view.latency, onClick: () => setView((v) => ({ ...v, latency: !v.latency })) },
            { label: 'Clip spikes', checked: clipOutliers, onClick: () => setClipOutliers((c) => !c) },
            { label: 'Overlay all hops', checked: overlay, onClick: () => setOverlay((o) => !o) },
            { label: 'Alerts', checked: alerts.on, onClick: () => setAlerts((a) => ({ ...a, on: !a.on })) },
          ],
        },
        {
          label: 'Tools', items: [
            { label: 'Quick trace 1.1.1.1 (Cloudflare)', onClick: () => addTargetHost('1.1.1.1') },
            { label: 'Quick trace 8.8.8.8 (Google)', onClick: () => addTargetHost('8.8.8.8') },
            { label: 'Quick trace 9.9.9.9 (Quad9)', onClick: () => addTargetHost('9.9.9.9') },
            { sep: true },
            { label: 'Edit config (selected)…', onClick: () => { if (sel) { setEditForm({ probe: sel.config.probe, timeout: sel.config.timeout, payload: sel.config.payload, maxhops: sel.config.maxhops, family: sel.config.family, src: sel.config.src }); setEditErrs({}); setEditing(true) } }, disabled: !sel },
          ],
        },
        {
          label: 'Help', items: [
            { label: 'About Net Pulse — Open Net Tools', onClick: () => setAbout(true) },
            { label: 'Project on GitHub', onClick: () => { try { window.open('https://github.com/ArchismanKarmakar/Net-Pulse-Open-Net-Tools', '_blank') } catch {} } },
          ],
        },
      ]} />
      {about && (
        <div className="about-overlay" onClick={() => setAbout(false)}>
          <div className="about-box" onClick={(e) => e.stopPropagation()}>
            <h2 style={{ margin: '0 0 6px' }}>Net Pulse — Open Net Tools</h2>
            <p style={{ margin: '0 0 10px', opacity: 0.75 }}>Cross-platform path-latency &amp; network diagnostics. Native C++ probe engine + Electron UI.</p>
            <button className="primary" onClick={() => setAbout(false)}>Close</button>
          </div>
        </div>
      )}
      <header>
        <h1 style={{ display: 'flex', alignItems: 'center', gap: '9px' }}>
          <svg width="24" height="24" viewBox="0 0 512 512" fill="none" xmlns="http://www.w3.org/2000/svg" aria-hidden="true" style={{ display: 'block', flexShrink: 0 }}>
            <defs>
              <linearGradient id="npPulse" x1="72" y1="256" x2="440" y2="256" gradientUnits="userSpaceOnUse">
                <stop offset="0" stopColor="#22d3ee"/><stop offset="0.55" stopColor="#38e0d6"/><stop offset="1" stopColor="#4f7cff"/>
              </linearGradient>
              <radialGradient id="npNode" cx="0.5" cy="0.5" r="0.5">
                <stop offset="0" stopColor="#eafcff"/><stop offset="0.4" stopColor="#5ff0ff"/><stop offset="1" stopColor="#22d3ee"/>
              </radialGradient>
            </defs>
            <rect x="16" y="16" width="480" height="480" rx="112" fill="#0c1524"/>
            <rect x="16.5" y="16.5" width="479" height="479" rx="111.5" fill="none" stroke="#22d3ee" strokeOpacity="0.16" strokeWidth="3"/>
            <g stroke="#22d3ee" fill="none">
              <circle cx="360" cy="196" r="46" strokeOpacity="0.30" strokeWidth="7"/>
              <circle cx="360" cy="196" r="80" strokeOpacity="0.16" strokeWidth="6"/>
            </g>
            <path d="M72 300 H176 L214 300 L246 176 L286 372 L318 256 L344 256" fill="none" stroke="url(#npPulse)" strokeWidth="26" strokeLinecap="round" strokeLinejoin="round"/>
            <circle cx="360" cy="196" r="30" fill="url(#npNode)"/>
          </svg>
          <span>Net&nbsp;Pulse <span style={{ opacity: 0.55, fontWeight: 500 }}>— Open Net Tools</span></span>
          <button className="themebtn" title="Toggle light / dark" onClick={() => setTheme((t) => t === 'light' ? 'dark' : 'light')}>{theme === 'light' ? '🌙' : '☀'}</button>
        </h1>
        <div className="controls">
          <input type="text" className="target" placeholder="host or IP (8.8.8.8, 2001:4860:4860::8888, example.com)"
            value={form.target} onChange={(e) => setForm({ ...form, target: e.target.value })}
            onKeyDown={(e) => e.key === 'Enter' && addTarget()} />
          <button className="primary" onClick={addTarget}>＋ Add</button>
          <label>Family<select value={form.family} onChange={(e) => setForm({ ...form, family: e.target.value })}>
            <option value="auto">Auto</option><option value="v4">IPv4</option><option value="v6">IPv6</option></select></label>
          <label title="Seconds between probes per hop (0.1–3600)">Probe<input type="number" min="0.1" max="3600" step="0.1" value={form.probe} onChange={(e) => setForm({ ...form, probe: e.target.value })} /></label>
          <label title="Route re-discovery interval, seconds (1–86400)">Trace<input type="number" min="1" max="86400" step="1" value={form.trace} onChange={(e) => setForm({ ...form, trace: e.target.value })} /></label>
          <label title="Per-probe timeout, seconds; 0 = auto (0–3600)">Timeout<input type="number" min="0" max="3600" step="0.1" value={form.timeout} onChange={(e) => setForm({ ...form, timeout: e.target.value })} /></label>
          <label title="ICMP payload bytes (0–65500; &gt;~1472 is fragmented)">Payload<input type="number" min="0" max="65500" step="1" value={form.payload} onChange={(e) => setForm({ ...form, payload: e.target.value })} /></label>
          <label title="Maximum hops / TTL (1–64)">Hops<input type="number" min="1" max="64" step="1" value={form.maxhops} onChange={(e) => setForm({ ...form, maxhops: e.target.value })} /></label>
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
          <label className="cb"><input type="checkbox" checked={clipOutliers} onChange={(e) => setClipOutliers(e.target.checked)} title="Keep the everyday latency band readable when a rate-limited hop spikes to thousands of ms" />Clip spikes</label>
        </div>
      </header>

      <div className="body">
        <aside style={{ width: sidebarWidth }} className="shrink-0 relative">
          <h3>Targets</h3>
          <ul>
            {targets.map((t) => {
              const st = targetState(t)
              const d = destHopOf(t)
              const isSel = sel && t.id === sel.id
              return (
                <li key={t.id} className={(isSel ? 'sel ' : '') + 's-' + st} onClick={() => { setSelId(t.id); setSelHop(null) }}>
                  <div className="flex items-center">
                    <span className="signal">
                      <span
                        className={'lamp st-' + destLamp(t)}
                        title={(() => {
                          const s = destLamp(t)
                          const m = { discovering: 'Route discovery in progress', settling: 'Destination healthy — latency/route changed recently, stabilising', ok: 'Destination healthy', okloss: 'Destination: occasional loss', warn: 'Destination: elevated latency or minor loss (70ms+ / PL>5%)', bad: 'Destination: high latency or loss (100ms+ / PL>10%)', down: 'Destination: unreachable, or severe latency/loss/jitter (150ms+ / PL>20% / 50ms+ swings)' }
                          return `Target: ${m[s] || s}`
                        })()}
                        aria-label={`target-${destLamp(t)}`}
                      />
                      <span
                        className={'lamp st-' + pathLamp(t)}
                        title={(() => {
                          const s = pathLamp(t)
                          const m = { discovering: 'Route discovery in progress', ok: 'Path healthy — packets routing cleanly to the target', warn: 'Path: an intermediate hop is dropping packets or not revealing itself', bad: 'Path: routing to the target pool via BGP, but the target or a router is dropping packets', down: 'Path: no route — internet/interface down, RTO, or first hop rejecting' }
                          return `Path: ${m[s] || s}`
                        })()}
                        aria-label={`path-${pathLamp(t)}`}
                      />
                    </span>
                    <span className="truncate flex-1">{t.name}</span>
                    {STATE_LABEL[st] && <span className={'statelabel st-' + st}>{STATE_LABEL[st]}</span>}
                    {/* Per-target pause/resume — selectively freeze one target */}
                    <button
                      className="card-pause"
                      title={t.paused ? 'Resume this target' : 'Pause this target'}
                      onClick={(e) => { e.stopPropagation(); pauseOne(t) }}
                    >{t.paused ? '▶' : '⏸'}</button>
                    {/* Delete button lives in the card so each target is self-contained */}
                    <button
                      className="card-del"
                      title="Remove target"
                      onClick={(e) => { e.stopPropagation(); ctrl(t.id, 'remove').then(() => { if (isSel) setSelId(null); refreshState() }) }}
                    >✕</button>
                  </div>
                  <small>{t.dest_ip} {t.family}{t.paused ? ' · paused' : ''}</small>
                  {d && (
                    <div className="grid grid-cols-3 gap-x-2.5 gap-y-0.5 mt-1.5 pt-1.5 border-t border-border text-[10px] font-mono text-muted">
                      <span>min <b className="text-ink">{fmt(d.min)}</b></span>
                      <span>avg <b className="text-ink">{fmt(d.avg)}</b></span>
                      <span>med <b className="text-ink">{fmt(d.med)}</b></span>
                      <span>max <b className="text-ink">{fmt(d.max)}</b></span>
                      <span>jit <b className="text-ink">{fmt(d.jitter)}</b></span>
                      <span>pl <b className={d.loss > 0 ? '' : 'text-ink'} style={d.loss > 0 ? { color: 'var(--bad)' } : undefined}>{d.loss.toFixed(0)}%</b></span>
                    </div>
                  )}
                </li>
              )
            })}
          </ul>
          {targets.length > 0 && (
            <div className="row">
              <button className={allPaused ? 'primary' : ''} onClick={() => pauseAll(!allPaused)}
                title="Pause or resume ALL targets">{allPaused ? '▶ Resume all' : '⏸ Pause all'}</button>
            </div>
          )}
          {/* ── Export / Import ─────────────────────────────────────────── */}
          <div className="sidebar-actions">
            <button onClick={exportTargetList} title="Save target list + config to a protected .npulse file (only NetPulse can open it)">⬇ Save list (.npulse)</button>
            <button onClick={exportTargetsJson} title="Export target list + config as human-readable JSON">⬇ Export JSON</button>
            <label className="import-btn" title="Load a .npulse target list (auto-starts tracing, skips exact duplicates)">
              ⬆ Load list (.npulse)
              <input id="np-import-input" type="file" accept=".npulse" style={{ display: 'none' }} onChange={(e) => { if (e.target.files[0]) importTargetList(e.target.files[0]); e.target.value = '' }} />
            </label>
          </div>
          <div
            onMouseDown={startDrag('sidebar')}
            title="Drag to resize"
            className="resize-handle-x absolute top-0 right-0 h-full w-1.5"
          />
        </aside>


        <main>
          {!sel ? <div className="empty">Add a target to begin.</div> : (
            <>
              <div className="statusbar">
                <b>{sel.name}</b> → {sel.dest_ip || 'resolving…'} <span className="badge">{sel.family}</span> · {sel.hops.length} hops
                {isDiscovering(sel) && !sel.error && (
                  <span className="badge inline-flex items-center align-middle" title="Discovering the route: resolving the destination and probing each hop. Each hop's first few real replies are used only to establish its address/route and aren't shown as stats yet, so an unrepresentative first reading isn't displayed as if it were typical. Clears per-hop as soon as real data is available.">
                    <span className="spinner sm" style={{ marginRight: 5 }} />
                    {sel.dest_ip ? 'discovering route…' : `resolving ${sel.name}…`}
                  </span>
                )}
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
                  <span className="divider" />
                  <span>probe <b>{sel.config.probe}s</b></span>
                  <span>timeout <b>{sel.config.timeout === 0 ? 'auto' : sel.config.timeout + 's'}</b></span>
                  <span className="divider" />
                  <span>payload <b>{sel.config.payload}B</b></span>
                  <span>max hops <b>{sel.config.maxhops}</b></span>
                  <span className="divider" />
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
                  <label title="ICMP payload bytes (0–65500)">Payload<input type="number" min="0" max="65500" step="1" value={editForm.payload} onChange={(e) => setEditForm({ ...editForm, payload: e.target.value })} /></label>
                  <label title="Maximum hops / TTL (1–64)">Max hops<input type="number" min="1" max="64" step="1" value={editForm.maxhops} onChange={(e) => setEditForm({ ...editForm, maxhops: e.target.value })} /></label>
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
                  {Object.keys(editErrs).length > 0 && (
                    <div className="cfg-errs" style={{ flexBasis: '100%', color: 'var(--danger)', fontSize: 12, marginTop: 4 }}>
                      {Object.values(editErrs).map((m, i) => <div key={i}>⚠ {m}</div>)}
                    </div>
                  )}
                </div>
              )}

              {sel.error ? (
                <div className="loading">
                  <div className="loading-text" style={{ color: 'var(--danger)' }}>⚠ {sel.error}</div>
                </div>
              ) : (
                <>
                  <div className="tablewrap" style={{ maxHeight: `${tablePct}%` }}>
                    <table>
                      <thead><tr>
                        <th>Hop</th><th>PL%</th><th>IP</th><th>Host</th><th>ASN</th><th>Network</th>
                        <th>Sent</th><th>Recv</th><th>Loss%</th><th>Cur</th><th title="Median — stays stable through a single spike, unlike Avg/Max">Med</th><th>Avg</th><th>Min</th><th>Max</th><th>Jitter</th>
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
                              <td className="font-semibold">{fmt(h.med)}</td><td>{fmt(h.avg)}</td><td>{fmt(h.min)}</td><td>{fmt(h.max)}</td><td>{fmt(h.jitter)}</td>
                              {view.trend && <td><Sparkline points={sel.series[String(h.hop)] || []} color={hopColor(i, h.hop === selHop)} /></td>}
                              {view.latency && <td className="latcell"><LatencyGraph h={h} prevAvg={prevAvg} nextAvg={nextAvg} scaleMax={scaleMax} alertMs={alerts.ms} /></td>}
                            </tr>
                          )
                        })}
                      </tbody>
                    </table>
                  </div>

                  <div onMouseDown={startDrag('table')} title="Drag to resize" className="resize-handle-y -my-1.5" />

                  {clippedSpikes > 0 && (
                    <div className="text-xs text-faint px-1 -mb-1">
                      ⓘ {clippedSpikes} sample{clippedSpikes > 1 ? 's' : ''} above {yDomain[1]} ms run off the top of the chart so the normal range stays readable —
                      <button className="ml-1.5 px-1.5 py-0 text-xs" onClick={() => setClipOutliers(false)}>show full range</button>
                    </div>
                  )}
                  <div className="chart" ref={chartRef}>
                    <ResponsiveContainer width="100%" height="100%">
                      <LineChart data={chartData} margin={{ top: 10, right: 20, bottom: 4, left: 0 }}>
                        <CartesianGrid stroke="var(--grid)" />
                        <XAxis dataKey="ts" type="number" domain={["dataMin","dataMax"]} tickFormatter={timeFmt} stroke="var(--axis)" tick={{ fill: "var(--axis)" }} fontSize={12} />
                        <YAxis stroke="var(--axis)" tick={{ fill: "var(--axis)" }} fontSize={12} domain={yDomain} allowDataOverflow label={{ value: "ms", angle: -90, position: "insideLeft", fill: "var(--axis)" }} />
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
import React, { useEffect, useMemo, useRef, useState } from 'react'
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, Legend,
} from 'recharts'
import * as bgp from './bgp'

import { FOCUS, CFG_LIMITS, validateCfg } from './lib/constants'
import { fmt, timeFmt, agoFmt, hopColor, latColor, niceCeil, download } from './lib/format'
import { api, ctrl, toolsApi } from './lib/api'
import MenuBar from './components/MenuBar'
// BUG FIX: this import used to exist here, pointing at
// ./components/Modal.jsx — a stale, pre-existing duplicate that predates
// the blocking/closing-animation work throughout this file. It was
// silently SHADOWED the entire time by the local `function Modal(...)`
// declared further down (first nested inside App(), now hoisted to module
// scope right below this comment) — that duplicate file was dead code,
// confirmed never actually rendered. Removed the import; the file itself
// is deleted too rather than left behind as a second, drifting copy of the
// same component (the exact class of confusion a stale duplicate
// PingPage.jsx already caused once earlier in this project).
import BgpDrawer, { RpkiBadge } from './components/BgpDrawer'
import Sparkline from './components/charts/Sparkline'
import LatencyGraph from './components/charts/LatencyGraph'
import PingPage from './components/tools/PingPage'
import { IconImage, IconCsv, IconRoute, IconBackupTable, IconTableChart, IconSave, IconUpload, IconDataObject } from './components/icons/MaterialIcons'
import DnsPage from './components/tools/DnsPage'
import PortScanPage from './components/tools/PortScanPage'
import TargetPanel from './components/TargetPanel'

// ── Top menu bar ────────────────────────────────────────────────────────────
// Lightweight click-to-open menus (File / Targets / View / Tools / Help). Each
// item is { label, onClick, checked?, sep?, disabled?, hint? }.


// Bumped by hand with every patched archive shipped for this project. Exists
// purely so "am I actually running the build I think I am" is a fact you can
// read off the About dialog instead of a guess — this repeatedly mattered
// while diagnosing issues where the fix was genuinely in the source but the
// person testing it was still on a stale build (Vite/Cargo incremental
// caching, or simply forgetting to fully rebuild after replacing files).
// package.json's own version stays "0.9.5" across these patches, so it
// cannot serve this purpose on its own.
const PATCH_BUILD = 'r54'

// BUG FIX: this used to be defined INSIDE App() (a nested function
// component). Every function declared inside another function is a
// genuinely NEW function value on every call — so on every single App
// re-render (which happens constantly: live probe data streaming in,
// often multiple times per second for an active session), Modal got a
// brand-new identity. React uses component TYPE identity to decide
// whether to preserve or remount an element across renders, so this
// meant React was unmounting and remounting the ENTIRE dialog on every
// App re-render while one was open — invisible before this file had
// any CSS entrance animation on the dialog (an instant recreation looks
// identical to not recreating it at all), but once one was added, every
// one of those remounts replayed it — producing exactly the "flashing"
// a live report described, specifically on dialogs likely to be open
// while a session is actively running (diagnostics, update-check,
// add-target prompts). Modal never actually closed over anything from
// App's own scope — every value it uses arrives via its own props — so
// it was always safe to hoist to module scope; it just never had been.
function Modal({ title, message, details, buttons = [], onClose, blocking = false, closing = false }) {
  // BUG FIX: the overlay used to close on ANY outside click unconditionally.
  // For a prompt the user genuinely must answer (protocol prerequisites),
  // that meant a stray click silently dismissed it and resolved the promise
  // with `false` — indistinguishable from a deliberate Cancel. `blocking`
  // opts a dialog out of click-outside dismissal entirely; it must then be
  // answered via one of its own buttons.
  return (
    <div className={'modal-overlay' + (closing ? ' closing' : '')} onClick={() => { if (!blocking) onClose(false) }}>
      <div className={'modal-box' + (closing ? ' closing' : '')} onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <h2 className="modal-title">{title}</h2>
          {/* FEATURE: an explicit close ("X") button — a live request
              specifically asked for both this AND a Cancel action button on
              the elevation/Npcap and IP-translation prompts. Shown
              regardless of `blocking`: blocking only guards against an
              ACCIDENTAL outside click resolving the dialog for you; a
              deliberate click on an explicit close control is a genuine,
              intentional choice, same category as clicking a real button,
              not the thing blocking exists to prevent. Resolves with
              `false` — the same value every existing caller's Cancel/decline
              branch already handles, so no call site needed to change to
              support this. */}
          <button className="modal-x" onClick={() => onClose(false)} aria-label="Close" title="Close">✕</button>
        </div>
        <div className="modal-message">{message}</div>
        {details && <div className="modal-details">{details}</div>}
        <div className="modal-actions">
          {buttons.map((btn, idx) => {
            // `kind` (new, optional) selects the semantic color; falls back
            // to the old primary/plain distinction when a caller hasn't
            // been updated to specify one yet, so this is backward
            // compatible with every existing showModal() call site.
            const cls = btn.kind === 'warning' ? 'btn-warning'
              : btn.kind === 'danger' ? 'btn-danger'
              : btn.primary ? 'primary'
              : ''
            return (
              <button key={idx} className={cls} onClick={() => onClose(btn.value)}>
                {btn.label}
              </button>
            )
          })}
        </div>
      </div>
    </div>
  )
}

export default function App() {
  const [form, setForm] = useState({ target: '', family: 'auto', probe: 1, trace: 30, timeout: 0, payload: 56, maxhops: 30, raw: true, protocol: 'icmp', destPort: 33434 })
  const [focusIdx, setFocusIdx] = useState(2) // Last 30 sec
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
  const [updateInfo, setUpdateInfo] = useState(null)
  const [updateBusy, setUpdateBusy] = useState(false)
  const [updateProgress, setUpdateProgress] = useState(null)
  const [updateDismissed, setUpdateDismissed] = useState(false)
  const [tool, setTool] = useState(null)
  const [tab, setTab] = useState('path') // 'path' (traceroute/MTR home) | 'ping' | 'dns' | 'ports'
  const [theme, setTheme] = useState(() => { try { return localStorage.getItem('np-theme') || 'dark' } catch { return 'dark' } })
  const chartRef = useRef(null)
  const [modal, setModal] = useState(null)
  const [modalClosing, setModalClosing] = useState(false)
  const [debugLogging, setDebugLogging] = useState(false)

  // Escape closes the About dialog. Added alongside removing its
  // click-outside-to-dismiss behaviour: a deliberate keypress is a clear
  // intent to close, whereas a stray click on the backdrop is not.
  useEffect(() => { console.info(`[NetPulse] patch build: ${PATCH_BUILD}`) }, [])
  const modalResolveRef = useRef(null)
  const showModal = ({ title, message, details, buttons = [{ label: 'OK', value: true, primary: true }], blocking = false }) => new Promise((resolve) => {
    modalResolveRef.current = resolve
    setModal({ title, message, details, buttons, blocking })
    // BUG FIX: this used to synthesize its own ad-hoc two-tone WebAudio
    // beep here, unconditionally, on EVERY showModal() call. Once
    // play_alert_sound() (the real OS-level notification sound) started
    // being called explicitly before specific showModal() invocations —
    // diagnostic logging, protocol prerequisites, update available — that
    // meant TWO different, unrelated sounds fired together every time:
    // this one, and the real OS one. Repeated live reports of "a very bad
    // sound" playing on these exact dialogs are consistent with hearing
    // both at once rather than the single, deliberate OS sound that was
    // actually intended. Removed entirely — any dialog that should make a
    // sound calls play_alert_sound() explicitly and gets exactly one,
    // genuinely the OS's own, chosen sound; a dialog that doesn't call it
    // (most of the 30+ showModal() sites in this file — export failures,
    // "nothing to export", etc.) now correctly stays silent instead of
    // always beeping regardless of how minor the message is.
  })
  const closeModal = (value) => {
    // BUG FIX / FEATURE: setModal(null) used to unmount the dialog the
    // instant a button was clicked — React removes it from the DOM on the
    // very next render, before any CSS transition on the way OUT has any
    // frame to actually play. An animated open with an instant, jarring
    // close reads as broken, not polished — exactly what Windows/macOS
    // dialogs don't do. Fixed with a short delayed-unmount: mark the
    // dialog as closing (Modal below swaps to a fade/scale-out class),
    // keep it mounted for exactly as long as that CSS transition takes,
    // THEN actually remove it from the tree.
    //
    // The promise is deliberately resolved AFTER that delay too, not
    // immediately — resolving early would let a caller's very next line
    // call showModal() again (several call sites do exactly this, e.g.
    // showing a follow-up error dialog) while THIS close's timeout is
    // still pending; that timeout would then fire mid-way through the
    // NEW dialog's own lifetime and incorrectly null it out from under
    // it. Delaying the resolution means a caller physically cannot show
    // the next dialog until this one has genuinely finished closing,
    // which removes the race instead of needing to guard against it.
    setModalClosing(true)
    setTimeout(() => {
      setModal(null)
      setModalClosing(false)
      if (modalResolveRef.current) {
        modalResolveRef.current(value)
        modalResolveRef.current = null
      }
    }, 160) // must match .modal-overlay.closing's transition-duration in styles.css
  }

  // ---- resizable panels (sidebar width, table/chart split) ----
  const [sidebarWidth, setSidebarWidth] = useState(() => { const v = Number(localStorage.getItem('np-sidebar-w')); return v >= 230 ? v : 300 })
  const [tablePct, setTablePct] = useState(() => { const v = Number(localStorage.getItem('np-table-pct')); return v >= 15 && v <= 70 ? v : 38 })
  // Which panel (if any) is actively being drag-resized right now. Purely
  // so the CSS can turn OFF the width transition for the duration of the
  // drag — see the `.target-panel.resizing` rule. That transition exists
  // for state-driven width changes (opening/closing a target, "in sync
  // with the dashboard animation"), not for a value that's being pushed
  // by raw mouse coordinates 60+ times a second; animating every one of
  // those over .32s is exactly what made dragging feel laggy, since the
  // rendered width was perpetually chasing the cursor instead of matching
  // it 1:1.
  const [resizing, setResizing] = useState(null) // null | 'sidebar' | 'table'
  // ---- dashboard filter/sort (target list) ----
  const [targetSearch, setTargetSearch] = useState('')
  // { done, total } while a fleet-wide Excel export is running, null
  // otherwise — drives the button label in TargetPanel.jsx (see
  // exportAllTargetsFullXlsx for why this is meaningful now: it's a real
  // per-target loop, not a single opaque call).
  const [xlsxExportProgress, setXlsxExportProgress] = useState(null)
  const [dashSortCol, setDashSortCol] = useState('status')
  const [dashSortDir, setDashSortDir] = useState(1) // 1 = asc-ish (worst/first first for status), -1 = flipped
  const dragRef = useRef(null)
  const dragRafRef = useRef(null)
  // Direct DOM handles for the two resizable elements. Writing to these
  // during a drag (see applyDrag below) is what keeps the drag from
  // triggering a React re-render on every frame — a re-render of App
  // cascades into every TargetCard in the list (each mounting Sparkline/
  // LatencyGraph/MiniVisualPath SVG subtrees), which is real work that has
  // nothing to do with the drag and is what made it feel laggy.
  const sidebarElRef = useRef(null)
  const tableWrapElRef = useRef(null)
  // The ONE hidden <input type="file"> for loading a .npulse list (rendered
  // once, near the end of this component — see the JSX below) — shared by
  // the File menu, the dashboard toolbar, and the compact sidebar, all of
  // which just call importInputRef.current?.click(). Previously each of
  // those three triggered it a different way (two separate native
  // label+hidden-input pairs, plus the File menu doing its own
  // document.getElementById('np-import-input')?.click()) — the dashboard
  // toolbar's input never actually had that id, so the File menu's lookup
  // silently found nothing and did nothing. One ref, one input, one click
  // path removes that whole class of "which surface has the id" bug rather
  // than just patching the missing one.
  const importInputRef = useRef(null)
  // Targets the <table> element itself, not its scrollable .tablewrap
  // wrapper — html-to-image renders the target node's own natural size
  // (via a cloned foreignObject), so capturing the table directly gets
  // every hop regardless of what's currently scrolled into view; capturing
  // the wrapper instead would crop to whatever's visible right now.
  const hopTableRef = useRef(null)
  // Applies the latest tracked mouse position exactly once per animation
  // frame, however many mousemove events fired since the last one, writing
  // straight to the DOM node's style — no setState, no React re-render,
  // so the edge tracks the cursor 1:1 regardless of list size. The final
  // value is committed to React state (and localStorage) once, in endDrag.
  const applyDrag = () => {
    dragRafRef.current = null
    const d = dragRef.current; if (!d) return
    if (d.kind === 'sidebar') {
      const w = Math.min(520, Math.max(230, d.startW + (d.lastX - d.startX)))
      d.value = w
      if (sidebarElRef.current) sidebarElRef.current.style.width = w + 'px'
    } else {
      const total = d.box ? d.box.clientHeight : 600
      const pct = Math.min(70, Math.max(15, d.startPct + ((d.lastY - d.startY) / total) * 100))
      d.value = pct
      if (tableWrapElRef.current) tableWrapElRef.current.style.maxHeight = pct + '%'
    }
  }
  const onDrag = (e) => {
    const d = dragRef.current; if (!d) return
    d.lastX = e.clientX; d.lastY = e.clientY
    if (dragRafRef.current == null) dragRafRef.current = requestAnimationFrame(applyDrag)
  }
  const endDrag = () => {
    if (dragRafRef.current != null) { cancelAnimationFrame(dragRafRef.current); dragRafRef.current = null }
    const d = dragRef.current
    dragRef.current = null
    setResizing(null)
    document.body.style.cursor = ''
    if (d && d.value != null) {
      if (d.kind === 'sidebar') {
        setSidebarWidth(d.value)
        localStorage.setItem('np-sidebar-w', String(d.value))
      } else {
        setTablePct(d.value)
        localStorage.setItem('np-table-pct', String(d.value))
      }
    }
    window.removeEventListener('mousemove', onDrag)
    window.removeEventListener('mouseup', endDrag)
  }
  const startDrag = (kind) => (e) => {
    e.preventDefault()
    dragRef.current = { kind, startX: e.clientX, startY: e.clientY, lastX: e.clientX, lastY: e.clientY, startW: sidebarWidth, startPct: tablePct, value: null, box: e.currentTarget.closest('main') }
    setResizing(kind)
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
    const baseGrace = Math.max(3.0, probe * 3) // at least 3s, or 3× the probe interval
    const extraIpv6 = t.family === 'v6' ? 2.0 : 0.0
    const grace = baseGrace + extraIpv6
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

  useEffect(() => {
    const t = setTimeout(async () => {
      const np = typeof window !== 'undefined' ? window.netpulse : null
      if (!np || !np.updater) return
      const info = await np.updater.check()
      if (info && info.available) setUpdateInfo(info)
    }, 3000)
    return () => clearTimeout(t)
  }, [])

  const checkForUpdatesManually = async () => {
    const np = typeof window !== 'undefined' ? window.netpulse : null
    if (!np || !np.updater) return
    const info = await np.updater.check()
    // BUG FIX: none of these three passed `blocking: true`, so a stray
    // click outside dismissed them same as the About dialog used to.
    // "Update available" also gets the same play_alert_sound()+in-app-
    // modal pairing already used for diagnostic logging — worth a genuine
    // attention sound; "No updates" and failures don't need one, they're
    // not actionable/urgent the same way.
    if (info && info.available) {
      setUpdateInfo(info); setUpdateDismissed(false)
      // BUG FIX: this used to check `np?.playAlertSound` — but `np` here is
      // `window.netpulse` directly, while playAlertSound lives on
      // `window.netpulse.tools` (what toolsApi() actually returns, and what
      // every OTHER call site in this file correctly uses). The check
      // silently evaluated to false every time, so this specific dialog
      // never made a sound at all — exactly the reported "no sound except
      // on diagnostics" symptom.
      const t = toolsApi()
      if (typeof t?.playAlertSound === 'function') t.playAlertSound('info').catch(() => {})
      await showModal({ title: 'Update available', message: `Version ${info.version} is ready to install.`, details: info.body || '', blocking: true })
    } else {
      await showModal({ title: 'No updates', message: "You're on the latest version.", blocking: true })
    }
  }

  const installUpdateNow = async () => {
    const np = typeof window !== 'undefined' ? window.netpulse : null
    if (!np || !np.updater || !updateInfo) return
    setUpdateBusy(true); setUpdateProgress(null)
    const res = await np.updater.install(updateInfo, (p) => setUpdateProgress(p))
    if (!res.ok) {
      setUpdateBusy(false)
      await showModal({ title: 'Update failed', message: res.error || 'Unknown error — please try again later or download the installer manually from GitHub.', blocking: true })
    }
    // On success the app relaunches itself (see updater.install in
    // tauri-bridge.js) — nothing further to do here.
  }

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

    // BUG FIX/FEATURE: destPort is now editable here, but Session::run_tcp()/
    // run_udp()/run_http() only ever read the destination port ONCE, at
    // start — confirmed directly in session.cpp: update_settings() there is
    // consumed by NOTHING in those three loops at all (only the ICMP-mode
    // run() loop ever checks rebuild_needed_). A live in-place port change
    // would silently do nothing useful: new probes would still target the
    // OLD port, while a growing pile of stale, port-specific hop-discovery
    // correlation state (port_to_ttl, in-flight probes, ICMP-registry
    // entries) from the throwaway old configuration just sits there. Hand-
    // rolling a proper in-place rebuild for three separate loops, under
    // time pressure, without being able to fully verify it, is a worse risk
    // than routing this through machinery that's ALREADY correct and
    // already tested: remove the target and add a fresh one with the new
    // settings. Functionally a full rebuild; implemented with zero new
    // engine-level state-reset logic to get wrong.
    const destPortChanged = (editForm.protocol === 'tcp' || editForm.protocol === 'udp' || editForm.protocol === 'http')
      && String(editForm.destPort ?? '') !== '' && Number(editForm.destPort) !== Number(sel.config?.destPort ?? '')

    if (destPortChanged) {
      const t = toolsApi()
      if (typeof t?.playAlertSound === 'function') t.playAlertSound('warning').catch(() => {})
      const choice = await showModal({
        title: 'Changing the remote port restarts this trace',
        message: `"${sel.name}" will be removed and re-added with port ${editForm.destPort} to apply this.`,
        details: <div>All current hop history and statistics for this target will be lost — this is a fresh start, not a resumed one. The other settings on this panel apply too.</div>,
        buttons: [
          { label: 'Cancel', value: 'cancel', kind: 'danger' },
          { label: 'Restart with new port', value: 'restart', kind: 'warning' },
        ],
        blocking: true,
      })
      if (choice !== 'restart') return
      try {
        await ctrl(sel.id, 'remove')
        // BUG FIX: this used to call addTargetHost(sel.name, {...overrides}) —
        // that function takes only ONE argument and hardcodes its own
        // defaults (family/probe/trace/timeout/payload/maxhops all fixed,
        // protocol/destPort not even included in its request at all), so a
        // second argument would have been silently ignored. The re-added
        // target would have come back as a plain ICMP trace with default
        // settings — not the TCP/UDP target with the new port the user
        // actually asked for. Building the /api/add request directly here,
        // matching the exact parameter set addTarget() (this file, the main
        // "+ Add" flow) already uses correctly, rather than misusing a
        // helper that was never meant for this.
        const q = new URLSearchParams({
          target: sel.name, family: editForm.family, probe: editForm.probe,
          trace: sel.config?.trace ?? 30, timeout: editForm.timeout,
          payload: editForm.payload, maxhops: editForm.maxhops, raw: '1',
          protocol: editForm.protocol || 'icmp', destPort: editForm.destPort || 33434,
        })
        if (editForm.src) q.set('src', editForm.src)
        const r = await api(`/api/add?${q}`, { method: 'POST' })
        if (r && r.id) { await refreshState(); selectTarget(r.id) }
        setEditing(false)
      } catch (e) {
        await showModal({ title: 'Could not restart trace', message: String(e && e.message || e), blocking: true })
      }
      return
    }

    const q = new URLSearchParams({ id: sel.id, probe: editForm.probe, timeout: editForm.timeout, payload: editForm.payload, maxhops: editForm.maxhops, family: editForm.family, src: editForm.src })
    try { await api(`/api/update?${q}`, { method: 'POST' }); setEditing(false) } catch {}
  }

  // Pause/resume probing of an individual hop (cuts network load for that hop).
  const toggleHopPause = async (hop) => {
    if (!sel) return
    const cur = new Set(sel.config?.pausedHops || [])
    cur.has(hop) ? cur.delete(hop) : cur.add(hop)
    const list = [...cur].sort((a, b) => a - b).join(',')
    try { await api(`/api/update?id=${sel.id}&pausedHops=${list}`, { method: 'POST' }); refreshState() } catch {}
  }

  const targets = state.targets || []
  const sel = targets.find((t) => t.id === selId)

  // Single source of truth for leaving the target-detail view and returning
  // to the full-width dashboard. Every "close"/"back" control in the detail
  // pane (the ✕ button, the "← Dashboard" link, Escape) calls this — not
  // setSelId(null) directly — so closing always fully resets the detail-only
  // UI state (selected hop, open config editor, open BGP drawer). Previously
  // some paths only cleared selId, which could leave a stale editing/drawer
  // state armed for whichever target was opened next.
  const closeTarget = () => {
    setSelId(null)
    setSelHop(null)
    setEditing(false)
    setEditForm(null)
    setDetail(null)
  }

  // Open a target's detail view (id) or return to the dashboard (null).
  // Passed to TargetPanel as a single callback so it doesn't need to know
  // about setSelId/setSelHop/closeTarget individually.
  const selectTarget = (id) => {
    if (id == null) { closeTarget(); return }
    setSelId(id); setSelHop(null)
  }

  // Remove a target from the dashboard table or sidebar strip. `wasSelected`
  // lets the sidebar strip close the detail view if the target being removed
  // is the one currently open; the dashboard table never has a selection to
  // clear, so it always passes false.
  const removeTarget = (id, wasSelected) => {
    ctrl(id, 'remove').then(() => { if (wasSelected) closeTarget(); refreshState() })
  }

  // Esc closes the open target detail view and returns to the dashboard —
  // the same keyboard convention as the BGP drawer and modals.
  useEffect(() => {
    if (!selId) return
    const onKey = (e) => { if (e.key === 'Escape') closeTarget() }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [selId])

  const isAlerting = (h) => alerts.on && ((h.avg != null && h.avg > alerts.ms) || (h.sent > 0 && h.loss > alerts.loss))

  const familyLabel = (actual, pref) => {
    if (!actual) return pref || 'auto'
    return pref && pref !== actual ? `${actual} (${pref})` : actual
  }

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

  // NMAP-style port-state word for a hop, TCP mode only. Distinct from
  // hopStatus() above (which drives the coloured dot and is about
  // performance/loss) — this is specifically about what kind of evidence a
  // TCP probe actually got, which "loss/latency" doesn't capture at all.
  // Only the DESTINATION hop can ever be "open" or "closed" — those come
  // from a real TCP-layer reply (SYN-ACK / RST) that only the real
  // endpoint can send; an intermediate hop's only possible TCP-mode
  // evidence is an ICMP Time-Exceeded, which has no open/closed concept,
  // so it can only ever be "responds" or "filtered" here.
  const tcpPortState = (h) => {
    if (h.is_dest) {
      if (h.tcp_port_open === true) return 'open'
      if (h.tcp_port_open === false) return 'closed'
      return h.sent > 0 && h.recv === 0 ? 'filtered' : null
    }
    return (h.sent > 0 && h.recv === 0) ? 'filtered' : null
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
  const STATE_LABEL = { discovering: 'discovering', ok: 'ok', okloss: 'loss', warn: 'path', bad: 'degraded', down: 'down' }

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

  // ── Manual/custom target order ──────────────────────────────────────────
  // Drag-and-drop and the ▲/▼ move buttons both write into this single
  // ordered array of target ids. Either interaction also flips the active
  // sort to 'custom' (both the sidebar dropdown and the dashboard table's
  // sort column) so the reordered position is immediately what's
  // displayed. The array is persisted so a manual arrangement survives
  // restarts.
  const [customOrder, setCustomOrder] = useState(() => {
    try { const v = JSON.parse(localStorage.getItem('np-custom-order') || '[]'); return Array.isArray(v) ? v : [] } catch { return [] }
  })
  useEffect(() => { try { localStorage.setItem('np-custom-order', JSON.stringify(customOrder)) } catch {} }, [customOrder])
  // Keep customOrder in sync with the live target set: newly-added targets
  // are appended at the end (so they don't silently jump to the top/bottom
  // of a manual arrangement), removed targets drop out. Keyed on the *set*
  // of ids (sorted+joined), not the targets array reference, so this only
  // runs when a target is actually added/removed — not on every 600ms poll
  // tick that merely refreshes hop stats.
  const targetIdKey = targets.map((t) => t.id).sort((a, b) => a - b).join(',')
  useEffect(() => {
    const liveIds = targets.map((t) => t.id)
    const live = new Set(liveIds)
    setCustomOrder((prev) => {
      const kept = prev.filter((id) => live.has(id))
      const missing = liveIds.filter((id) => !kept.includes(id))
      if (missing.length === 0 && kept.length === prev.length) return prev
      return [...kept, ...missing]
    })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [targetIdKey])
  const orderIndex = (id) => { const i = customOrder.indexOf(id); return i === -1 ? Number.MAX_SAFE_INTEGER : i }
  // Drag-to-reorder — Motion's <Reorder.Group> (TargetPanel.jsx) owns the
  // actual drag gesture and hands back the fully reordered `rows` array
  // directly; this just needs to fold that back into customOrder (the
  // persisted ordering — see above) and switch to custom sort, same as the
  // old moveTarget()/reorderTo() did. Previously this whole area was a
  // hand-rolled mouse-event drag implementation (raw mousemove tracking,
  // live DOM sibling queries, a requestAnimationFrame busy-gate) — replaced
  // entirely rather than patched again, since Tauri's webview intercepting
  // native HTML5 drag events was never actually the hard part; getting a
  // robust drag GESTURE right (distinguishing a tap from a drag, handling
  // fast multi-item moves, keeping displaced rows animating smoothly) is a
  // solved problem in a maintained library and isn't worth re-solving by
  // hand. Motion's Reorder is pointer-event-based internally, so it isn't
  // affected by Tauri's native-dragstart interception either.
  //
  // newRows only contains whatever's currently visible (filtered/sorted) —
  // ids not in newRows (hidden by a search filter, say) keep their existing
  // relative position, appended after the reordered visible ones, same
  // fallback behavior the old reorderTo() had for an id it didn't know about.
  const handleReorder = (newRows) => {
    const newIds = newRows.map((t) => t.id)
    const newIdSet = new Set(newIds)
    setCustomOrder((prev) => {
      const rest = prev.filter((id) => !newIdSet.has(id))
      return [...newIds, ...rest]
    })
    setDashSortCol('custom')
  }

  // Single-step reorder for the card's ▲/▼ buttons — a keyboard/click-
  // friendly alternative to the drag gesture above, not a separate
  // mechanism: it swaps the target with whichever neighbor is currently
  // adjacent to it in the VISIBLE (sorted/filtered) list, then folds that
  // back into customOrder and switches to custom order, exactly like
  // handleReorder does for a drag. (Previously these buttons called a
  // bare `moveTarget()` that no longer existed after drag-to-reorder was
  // rebuilt on customOrder/handleReorder — a leftover reference from
  // before that migration, silently throwing a ReferenceError on click
  // and never actually moving anything.)
  const moveTarget = (id, dir) => {
    const ids = dashboardRows.map((t) => t.id)
    const i = ids.indexOf(id)
    if (i === -1) return
    const j = i + dir
    if (j < 0 || j >= ids.length) return
    const newIds = ids.slice()
    ;[newIds[i], newIds[j]] = [newIds[j], newIds[i]]
    setCustomOrder((prev) => {
      const movedSet = new Set(newIds)
      const rest = prev.filter((pid) => !movedSet.has(pid))
      return [...newIds, ...rest]
    })
    setDashSortCol('custom')
  }

  // ── Dashboard: summary counts + filtered/sorted list ────────────────────
  // Two separate counts — one per lamp — instead of one combined status,
  // since destination health and path health are genuinely different
  // signals (see targetState/destLamp/pathLamp above) and collapsing them
  // into a single number hides which one is actually the problem.
  const lampSummary = (lampFn) => {
    const counts = { total: targets.length, ok: 0, settling: 0, warn: 0, bad: 0, down: 0, discovering: 0 }
    for (const t of targets) {
      const s = lampFn(t)
      if (s === 'discovering') counts.discovering++
      else if (s === 'settling') counts.settling++
      else if (s === 'down') counts.down++
      else if (s === 'bad') counts.bad++
      else if (s === 'warn') counts.warn++
      else counts.ok++ // ok / okloss
    }
    return counts
  }
  const destSummary = useMemo(() => lampSummary(destLamp), [targets, alerts])
  const pathSummary = useMemo(() => lampSummary(pathLamp), [targets, alerts])
  const lampBucket = (lampFn, t) => {
    const s = lampFn(t)
    return ['discovering', 'settling', 'down', 'bad', 'warn'].includes(s) ? s : 'ok'
  }
  // Clicking a summary card filters the list to that lamp+bucket; clicking
  // the same one again clears it. { dim: 'dest'|'path', status: 'ok'|'settling'|'warn'|'bad'|'down'|'discovering' }
  const [statusFilter, setStatusFilter] = useState(null)
  const toggleStatusFilter = (dim, status) => {
    setStatusFilter((f) => (f && f.dim === dim && f.status === status) ? null : { dim, status })
  }

  const filteredTargets = useMemo(() => {
    const q = targetSearch.trim().toLowerCase()
    let list = !q ? targets : targets.filter((t) =>
      t.name.toLowerCase().includes(q) || (t.dest_ip || '').toLowerCase().includes(q))
    if (statusFilter) {
      const lampFn = statusFilter.dim === 'dest' ? destLamp : pathLamp
      list = list.filter((t) => lampBucket(lampFn, t) === statusFilter.status)
    }
    return list
  }, [targets, targetSearch, statusFilter])


  const STATUS_RANK = { down: 0, bad: 0, warn: 1, okloss: 1, discovering: 2, settling: 2, ok: 3 }


  // direction), distinct from the sidebar's dropdown sort above since a real
  // table's UX is "click the header", not "pick from a select".
  const dashboardRows = useMemo(() => {
    return [...filteredTargets].sort((a, b) => {
      const da = destHopOf(a), db = destHopOf(b)
      let cmp = 0
      switch (dashSortCol) {
        case 'custom':
          cmp = orderIndex(a.id) - orderIndex(b.id)
          break
        case 'status': {
          const ra = STATUS_RANK[targetState(a)] ?? 4, rb = STATUS_RANK[targetState(b)] ?? 4
          cmp = ra - rb
          break
        }
        case 'latency': {
          const la = da ? (da.med ?? da.avg) : null, lb = db ? (db.med ?? db.avg) : null
          if (la == null && lb == null) cmp = 0
          else if (la == null) cmp = 1
          else if (lb == null) cmp = -1
          else cmp = la - lb
          break
        }
        case 'loss':
          cmp = (da ? da.loss : -1) - (db ? db.loss : -1)
          break
        case 'jitter': {
          const ja = da ? da.jitter : null, jb = db ? db.jitter : null
          if (ja == null && jb == null) cmp = 0
          else if (ja == null) cmp = 1
          else if (jb == null) cmp = -1
          else cmp = ja - jb
          break
        }
        case 'hops':
          cmp = (a.hops?.length || 0) - (b.hops?.length || 0)
          break
        default:
          cmp = a.name.localeCompare(b.name)
      }
      if (cmp === 0) cmp = a.name.localeCompare(b.name)
      // Custom order has no "direction" to flip — it's a manually placed
      // sequence, not a sortable column with a header to click twice. Any
      // dashSortDir left over from a PREVIOUS column sort (e.g. someone
      // clicked "Latency" twice for descending, then dragged a card) would
      // otherwise silently reverse what's rendered relative to what
      // customOrder's array actually says, while handleReorder folds
      // whatever's CURRENTLY rendered straight back into customOrder — a
      // mismatch that's invisible with 2 rows (a reversed swap still just
      // trades the same two rows) but corrupts the persisted order with 3+.
      return dashSortCol === 'custom' ? cmp : cmp * dashSortDir
    })
  }, [filteredTargets, dashSortCol, dashSortDir, alerts, customOrder])

  const toggleDashSort = (col) => {
    if (dashSortCol === col) setDashSortDir((d) => -d)
    else { setDashSortCol(col); setDashSortDir(1) }
  }
  // Used by the compact panel's <select> — always sets the column outright
  // (a dropdown pick isn't a "click the same header again" gesture).
  const selectDashSort = (col) => { setDashSortCol(col); setDashSortDir(1) }


  useEffect(() => { if (sel && selHop === null) { const d = sel.hops.find((h) => h.is_dest); if (d) setSelHop(d.hop) } }, [sel, selHop])

  // ASN/network enrichment for public hop IPs (cached; only refetches on change)
  const ipKey = useMemo(() => sel ? [...new Set(sel.hops.map((h) => h.address).filter(bgp.isPublicIp))].sort().join(',') : '', [sel])

  // Reverse-DNS hostnames, resolved in the Electron main process (Node dns) so
  // the probe engine never blocks on getnameinfo. Cached per IP; results fill
  // the HOST column.
  // Same unbounded-growth shape as bgp.js's RIPEstat cache (see its comment)
  // — one entry per distinct hop IP ever seen, for the life of the page.
  // Capped with the same simple FIFO eviction for the same reason: cheap,
  // and anything still relevant just gets re-resolved on its next lookup.
  const HOST_CACHE_MAX = 500
  const capObject = (obj, max) => {
    const keys = Object.keys(obj)
    if (keys.length <= max) return obj
    for (const k of keys.slice(0, keys.length - max)) delete obj[k]
    return obj
  }
  const hostCacheRef = useRef({})
  const [hostCache, setHostCache] = useState({})
  useEffect(() => {
    const rev = (typeof window !== 'undefined' && window.netpulse && window.netpulse.tools && window.netpulse.tools.reverse)
    if (!sel || !rev) return
    const ips = [...new Set(sel.hops.map((h) => h.address))].filter((ip) => ip && bgp.isPublicIp(ip) && !(ip in hostCacheRef.current))
    if (!ips.length) return
    let alive = true
    ;(async () => {
      for (const ip of ips) {
        hostCacheRef.current[ip] = '' // mark in-flight so we don't re-request
        try { const r = await rev(ip); hostCacheRef.current[ip] = (r && r.names && r.names[0]) || '' } catch { hostCacheRef.current[ip] = '' }
        capObject(hostCacheRef.current, HOST_CACHE_MAX)
        if (alive) setHostCache({ ...hostCacheRef.current })
      }
    })()
    return () => { alive = false }
  }, [ipKey])
  const hostOf = (h) => h.hostname || hostCache[h.address] || h.stale_hostname || ''
  useEffect(() => {
    if (!ipKey) return
    ipKey.split(',').filter(Boolean).forEach(async (ip) => {
      if (asn[ip]) return
      setAsn((a) => capObject({ ...a, [ip]: { loading: true } }, HOST_CACHE_MAX))
      const info = await bgp.ipInfo(ip)
      // Same cap as hostCacheRef above — asn accumulates one entry per
      // distinct public hop IP for the life of the page otherwise.
      setAsn((a) => capObject({ ...a, [ip]: { ...info, loading: false } }, HOST_CACHE_MAX))
    })
  }, [ipKey]) // eslint-disable-line

  // Quick-add a host directly (menu shortcuts) using default config, without
  // touching the add form. Skips exact-duplicate hosts already being traced.
  const addTargetHost = async (host) => {
    host = String(host || '').trim()
    if (!host) return
    if (targets.some((t) => t.name === host)) { const ex = targets.find((t) => t.name === host); if (ex) selectTarget(ex.id); return }
    const q = new URLSearchParams({ target: host, family: 'auto', probe: 1, trace: 30, timeout: 0, payload: 56, maxhops: 30, raw: '1' })
    const r = await api(`/api/add?${q}`, { method: 'POST' })
    if (r && r.id) { await refreshState(); selectTarget(r.id) }
  }

  // Shared by the protocol dropdown's own onChange (immediate feedback the
  // moment UDP/TCP is picked, before a host is even typed) and addTarget()
  // below (a final, authoritative re-check before actually adding). Returns
  // true if it's fine to proceed, false if it showed a blocking modal.
  // `hostForMessage` is cosmetic only — falls back to a generic phrase when
  // called from the dropdown, where the target field may still be empty.
  // Shared by the protocol dropdown's own onChange (immediate feedback the
  // moment UDP/TCP is picked, before a host is even typed) and addTarget()
  // below (a final, authoritative re-check before actually adding). Returns
  // true if it's fine to proceed, false if it showed a blocking modal.
  // `hostForMessage` is cosmetic only — falls back to a generic phrase when
  // called from the dropdown, where the target field may still be empty.
  // `onDecline`, if given, runs when the user closes the modal without
  // resolving the issue (Cancel, or the elevation attempt itself failing) —
  // the dropdown's own onChange uses this to snap the selection back to
  // ICMP rather than leave it sitting on a protocol that can't work.
  const checkProtocolPrerequisites = async (proto, hostForMessage, onDecline) => {
    if (proto === 'icmp') return true
    let caps = null
    let probeFailed = false
    const capsFn = toolsApi()?.capabilities
    if (typeof capsFn !== 'function') {
      // BUG FIX: `toolsApi()?.capabilities?.()` on a missing method throws
      // NOTHING — optional chaining just evaluates the whole expression to
      // undefined and moves on. That means a stale build (this page loaded
      // an older bundled tauri-bridge.js that predates the capabilities
      // command existing at all) produced ZERO trace anywhere: no thrown
      // exception for a catch block to see, no console output, nothing —
      // completely indistinguishable from "checked, all fine". Checking the
      // function actually exists BEFORE calling it, and logging loudly if
      // not, is what actually surfaces that class of failure.
      probeFailed = true
      console.error('[NetPulse] toolsApi().capabilities is not a function — this build\'s frontend bridge does not have the capability probe. Protocol prerequisite gating is DISABLED.', toolsApi())
    } else {
      try {
        caps = await capsFn()
      } catch (e) {
        probeFailed = true
        // BUG FIX: this used to be a silent `catch { caps = null }` with no
        // trace anywhere — the capability probe failing for ANY reason (a
        // rejected invoke(), a thrown exception in the Rust/C++ command, a
        // malformed JSON response) was completely indistinguishable from
        // "checked, and everything's fine", so the entire feature could go
        // dark with zero visible symptom. That is exactly consistent with a
        // live report of "it never shows any prompt or alert at all" — this
        // makes a genuine probe failure loud (devtools console) instead of
        // silent, so it's at least possible to tell the two cases apart.
        console.error('[NetPulse] capabilities() check failed — protocol prerequisite gating is DISABLED for this attempt:', e)
      }
    }
    // If the probe itself fails, don't BLOCK the caller on it (a broken
    // capability check must never prevent someone from adding a target that
    // would otherwise work fine) — but DO leave a visible, low-friction trace
    // beyond the console, since "fails silently forever" is a worse failure
    // mode than "occasionally shows one extra dismissible notice".
    if (probeFailed || !caps) {
      if (probeFailed) {
        showModal({
          title: 'Could not check protocol requirements',
          message: `The setup check for ${proto.toUpperCase()} failed to run.`,
          details: <div>Proceeding without it — {proto.toUpperCase()} may or may not actually work here. Open DevTools (F12) for the specific error if this keeps happening.</div>,
        }) // deliberately not awaited — this is informational, not blocking
      }
      return true
    }
    const needsElevation = !caps.elevated
    const needsCapture = proto === 'tcp' && caps.platform === 'windows' && !caps.capture
    if (!(needsElevation || needsCapture)) return true

    const NPCAP_URL = 'https://npcap.com/#download'
    const t = toolsApi()
    const isWin = caps.platform === 'windows'
    const P = proto.toUpperCase()

    // Use the NATIVE OS dialog for these prompts, not the in-app React modal.
    // Three concrete reasons, all reported as real problems with the modal:
    //   1. The modal's overlay dismissed on any outside click, so a prompt the
    //      user MUST answer could be waved away by a stray click.
    //   2. It played no sound at all — a native dialog uses the platform's own
    //      alert sound and warning/error icon.
    //   3. It is only as portable as its own CSS; the native dialog looks and
    //      behaves correctly on Windows, macOS and Linux for free.
    // Falls back to the in-app modal if the native dialog is unavailable for
    // any reason (older bridge build, permission not granted), so this can
    // never silently show nothing — the failure mode that started all this.
    const lines = []
    if (needsElevation) {
      lines.push(`• Administrator rights are required for ${P}.`)
      lines.push(`  This app is not currently running elevated, and receiving the ICMP replies that ${P} hop discovery depends on needs them.`)
    }
    if (needsCapture) {
      lines.push('• Npcap (or WinPcap) must be installed for TCP.')
      lines.push('  TCP hop discovery needs a packet-capture driver to observe the ICMP replies — the same requirement PingPlotter, Nmap and tcptraceroute all have.')
      lines.push('  Npcap also ships with Nmap: if Nmap is already installed, re-run its installer and tick the Npcap component.')
      lines.push(`  Download: ${NPCAP_URL}`)
    }
    lines.push('')
    lines.push(`ICMP needs none of this and works right now.${proto === 'tcp' ? ' TCP still measures the destination correctly without a capture driver — only the intermediate hops are affected.' : ''}`)
    const body = lines.join('\n')
    const title = `${P} needs setup first`

    // BUG FIX: this used to prefer a NATIVE OS dialog (nativeAsk/
    // nativeMessage) whenever available, falling back to the in-app modal
    // below only if native dialogs weren't available at all. That gave the
    // OS's own alert sound and true modality "for free" — but native
    // dialogs render at whatever font size the OS/theme dictates, which
    // this app has no way to influence at all; a direct request to make
    // this text larger is simply impossible to satisfy while still using
    // one. The in-app modal below already has everything else a native
    // dialog would have provided: `blocking: true` (genuinely un-
    // dismissable by outside click, same guarantee), and — paired with
    // play_alert_sound() — the real OS alert sound too, without giving up
    // control over its own styling. So it's now simply the only path,
    // rather than a fallback for when the "better" path isn't available.

    // ---- In-app modal (styled, resizable, with a real OS alert sound) ----
    const openNpcap = (e) => { e.preventDefault(); try { window.open(NPCAP_URL, '_blank') } catch {} }
    const reasons = []
    if (needsElevation) {
      reasons.push(
        <div key="elev" style={{ marginBottom: 6 }}>
          • <b>Administrator rights are required for {P}.</b> This app is not currently elevated, and receiving the ICMP replies that {P} hop discovery depends on needs them.
        </div>
      )
    }
    if (needsCapture) {
      reasons.push(
        <div key="pcap" style={{ marginBottom: 6 }}>
          • <b>Npcap (or WinPcap) must be installed for TCP.</b> TCP hop discovery needs a packet-capture driver to observe the ICMP replies — the same requirement PingPlotter, Nmap and tcptraceroute all have. Npcap also ships with <b>Nmap</b>, so if Nmap is installed you may just need to re-run its installer and tick the Npcap component.{' '}
          <a href={NPCAP_URL} target="_blank" rel="noreferrer" onClick={openNpcap}>Download Npcap</a>
        </div>
      )
    }
    const buttons = [{ label: 'Cancel', value: 'cancel', kind: 'danger' }]
    if (needsElevation && isWin) buttons.push({ label: 'Restart as Administrator', value: 'elevate', kind: 'danger' })
    if (needsCapture) buttons.push({ label: 'Download Npcap…', value: 'download', kind: 'warning' })
    if (proto === 'tcp' && !needsElevation && needsCapture) buttons.push({ label: 'Continue anyway (destination only)', value: 'anyway', kind: 'warning' })
    const target = (hostForMessage || '').trim()
    // BUG FIX/FEATURE: this used to always play the same 'warning' sound
    // regardless of which prerequisite was missing. A live report
    // specifically asked for the more severe OS "critical" sound
    // (MB_ICONHAND on Windows — see play_alert_sound(), netpulse_ffi.cpp)
    // when administrator elevation is what's needed, distinct from the
    // milder tier used for "you need to install Npcap" — elevation is the
    // more consequential of the two (it restarts the whole app), and
    // needsElevation is checked first/is the more common case reaching
    // this function at all, so it takes priority when both happen to be
    // true at once.
    if (typeof t?.playAlertSound === 'function') t.playAlertSound(needsElevation ? 'error' : 'warning').catch(() => {})
    const choice = await showModal({
      title,
      message: target ? `"${target}" was not added.` : `${P} tracing needs one more thing before it can discover a path.`,
      details: (
        <div>
          {reasons}
          <div style={{ marginTop: 8, opacity: 0.8 }}>
            ICMP needs none of this and works right now. {proto === 'tcp' ? 'TCP still measures the destination correctly without a capture driver — only the intermediate hops are affected.' : ''}
          </div>
        </div>
      ),
      buttons,
      blocking: true, // must be answered — no click-outside dismiss
    })
    if (choice === 'download') {
      try { window.open(NPCAP_URL, '_blank') } catch {}
      if (onDecline) onDecline()
      return false
    }
    if (choice === 'elevate') {
      try { await toolsApi().relaunchElevated() }
      catch (e) {
        await showModal({ title: 'Could not restart elevated', message: String(e && e.message || e), details: <div>You can also close the app and start it again with “Run as administrator”.</div>, blocking: true })
        if (onDecline) onDecline()
      }
      return false
    }
    if (choice === 'anyway') return true
    // 'cancel', or the modal dismissed some other way — nothing resolved.
    if (onDecline) onDecline()
    return false
  }

  const addTarget = async () => {
    if (!form.target.trim()) return

    // ── Config validation ────────────────────────────────────────────────────
    const cfgErrs = validateCfg(form)
    if (Object.keys(cfgErrs).length) {
      await showModal({
        title: 'Invalid configuration',
        message: 'Please fix the config before adding the target.',
        details: <div>{Object.values(cfgErrs).map((m, i) => <div key={i}>• {m}</div>)}</div>,
      })
      return
    }

    // ── Prerequisite check ──────────────────────────────────────────────
    // Refuse to add a target that physically cannot report a path, rather than
    // adding it and letting it spin in "discovering" forever with no
    // explanation — which is exactly how these limitations used to surface.
    // Re-checked here (not just trusted from the proactive dropdown check
    // below) since capabilities can change between selecting a protocol
    // and pressing Add (elevating in another window, installing Npcap), and
    // this is also the only check reached if the protocol arrived some
    // other way — Load list, a pasted config — rather than through the
    // dropdown's own onChange.
    if (!(await checkProtocolPrerequisites(form.protocol || 'icmp', form.target.trim(), () => setForm((f) => ({ ...f, protocol: 'icmp' }))))) return

    // ── Duplicate check ──────────────────────────────────────────────────────
    // A target is a "duplicate" when the same host name/IP is already being
    // traced WITH the exact same config. If at least one config field differs
    // (probe interval, timeout, payload size, max hops, raw mode, family,
    // source interface, protocol, or destination port) the new entry is
    // allowed — different configs produce meaningfully different measurement
    // sets (e.g. IPv4 vs IPv6, two payload sizes for path-MTU checks, two
    // probe rates for comparison, or ICMP vs TCP vs UDP against the same
    // host, which can behave completely differently — a firewall that drops
    // ICMP but allows TCP:443 is a common, useful case to compare directly).
    // Identity is (host, protocol, port) — nothing else. Two traces that differ
    // only in probe rate or payload measure the same thing twice and just split
    // the paced budget, so they are treated as duplicates now. Genuinely
    // different measurements still coexist freely: ICMP vs TCP vs UDP to one
    // host, and TCP:443 vs TCP:80, are all distinct entries.
    const protoOf = (c) => (c?.protocol || 'icmp')
    // Port is only part of the identity where it actually varies the probe;
    // ICMP has no port, so every ICMP trace to a host is the same trace.
    const portOf = (c) => (protoOf(c) === 'icmp' ? '' : String(c?.destPort ?? ''))
    const formCfg = { protocol: form.protocol, destPort: form.destPort }
    const dupe = targets.find((t) => (
      t.name === form.target.trim() &&
      protoOf(t.config) === protoOf(formCfg) &&
      portOf(t.config) === portOf(formCfg)
    ))
    if (dupe) {
      await showModal({
        title: 'Already in the list',
        message: `"${form.target.trim()}" is already being traced with ${(form.protocol || 'icmp').toUpperCase()}${form.protocol && form.protocol !== 'icmp' ? ':' + form.destPort : ''}.`,
        details: <div>Each host is listed once per protocol and port. Pick a different protocol or port (for example TCP:80 alongside TCP:443), or select the existing entry in the list.</div>,
      })
      return
    }
    // ─────────────────────────────────────────────────────────────────────────

    // Auto-translate / family-mismatch handling for literal IPs.
    let targetText = form.target.trim()
    const isIPv4Literal = /^\d{1,3}(?:\.\d{1,3}){3}$/.test(targetText)
    const isIPv6Literal = targetText.includes(':') && !targetText.includes(' ')
    if (isIPv4Literal && form.family === 'v6') {
      // BUG FIX: this modal offers a genuine choice (translate vs. fall
      // back to v4) via two buttons, but without `blocking: true` an
      // ACCIDENTAL outside click resolved the promise with `false` — the
      // exact same value clicking "Use IPv4 instead" produces — so a stray
      // click silently made a real decision on the user's behalf and
      // addTarget() carried on to actually add the target under it. Adding
      // `blocking: true` means only a genuine button click can resolve
      // this at all; an outside click now does nothing, matching the
      // About dialog's own already-fixed behavior.
      // Sound: same significance as the protocol-prerequisite prompts —
      // this genuinely changes what gets added, not an informational aside.
      { const t = toolsApi(); if (typeof t?.playAlertSound === 'function') t.playAlertSound('warning').catch(() => {}) }
      const choice = await showModal({
        title: 'IPv4 address selected with IPv6 mode',
        message: 'You entered an IPv4 address but selected IPv6.',
        details: <div>Translate to an IPv4-mapped IPv6 address (::ffff:a.b.c.d) and probe as IPv6, or fall back to IPv4 for this target?</div>,
        buttons: [
          { label: 'Cancel', value: 'cancel', kind: 'danger' },
          { label: 'Use IPv4 instead', value: 'fallback', primary: true },
          { label: 'Translate to IPv6', value: 'translate', kind: 'warning' },
        ],
        blocking: true,
      })
      // BUG FIX: this used to be a plain boolean (true = translate, false =
      // fall back AND STILL PROCEED to add the target). That meant there
      // was no way to genuinely cancel the add — declining the translation
      // wasn't "stop", it was "do something else instead but keep going" —
      // and the newly-added X button, which universally resolves `false`,
      // would have silently landed on that same "keep going" path instead
      // of actually canceling, exactly the opposite of what clicking X
      // should do. Three explicit outcomes now — 'cancel' (X maps here too,
      // see the guard below) genuinely aborts with nothing added; the other
      // two are real, meaningful choices, not stand-ins for cancel.
      if (choice === 'cancel' || choice === false) return
      if (choice === 'translate') {
        targetText = '::ffff:' + targetText
      } else {
        // fallback to IPv4 family
        form.family = 'v4'
        setForm({ ...form, family: 'v4' })
      }
    }
    if (isIPv6Literal && form.family === 'v4') {
      // If it's an IPv4-mapped IPv6 address, allow translating back to IPv4.
      const m = targetText.match(/::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/)
      if (m) {
        // Same bug, same fix as the block above.
        { const t = toolsApi(); if (typeof t?.playAlertSound === 'function') t.playAlertSound('warning').catch(() => {}) }
        const choice = await showModal({
          title: 'IPv6 address selected with IPv4 mode',
          message: 'You entered an IPv6 address that embeds an IPv4 address.',
          details: <div>Translate to the embedded IPv4 and probe as IPv4, or use IPv6 for this target instead?</div>,
          buttons: [
            { label: 'Cancel', value: 'cancel', kind: 'danger' },
            { label: 'Use IPv6 instead', value: 'fallback', primary: true },
            { label: 'Translate to IPv4', value: 'translate', kind: 'warning' },
          ],
          blocking: true,
        })
        if (choice === 'cancel' || choice === false) return
        if (choice === 'translate') {
          targetText = m[1]
        } else {
          form.family = 'v6'
          setForm({ ...form, family: 'v6' })
        }
      } else {
        // Cannot translate arbitrary IPv6 → IPv4; switch family to v6.
        // BUG FIX: this one didn't even capture a return value before —
        // `await showModal(...)` with no `if`/`else` around what follows
        // meant the family switch and continuation ran completely
        // unconditionally, so there was never actually a "cancel" here at
        // all regardless of blocking. There genuinely is only one sensible
        // outcome (a generic IPv6 address cannot become IPv4), so
        // continuing either way IS correct — `blocking: true` here is
        // purely to guarantee the person actually reads this once, not to
        // change what happens next.
        { const t = toolsApi(); if (typeof t?.playAlertSound === 'function') t.playAlertSound('warning').catch(() => {}) }
        await showModal({
          title: 'Cannot translate to IPv4',
          message: 'Cannot translate a generic IPv6 address to IPv4.',
          details: <div>The trace will use IPv6 family instead.</div>,
          buttons: [{ label: 'OK', value: true, primary: true }],
          blocking: true,
        })
        form.family = 'v6'
        setForm({ ...form, family: 'v6' })
      }
    }

    const q = new URLSearchParams({ target: targetText, family: form.family, probe: form.probe, trace: form.trace, timeout: form.timeout, payload: form.payload, maxhops: form.maxhops, raw: form.raw ? '1' : '0', protocol: form.protocol || 'icmp', destPort: form.destPort || 33434 })
    if (iface) q.set('src', iface)
    const r = await api(`/api/add?${q}`, { method: 'POST' })
    setForm({ ...form, target: '' })
    if (r.id) {
      await refreshState() // ensure `targets` contains the new one before selecting it
      selectTarget(r.id)
    }
  }

  const openTool = (name) => {
    setTool(name)
  }

  const closeTool = () => setTool(null)

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
  // Sorted by customOrder (not raw creation order) so Save list / Export
  // JSON reflect whatever arrangement the user actually left the sidebar
  // in — orderIndex() already falls back to "end of list" for any id that
  // somehow isn't in customOrder, so this is safe even if that array is
  // momentarily out of sync with `targets`.
  const targetListPayload = () => [...targets].sort((a, b) => orderIndex(a.id) - orderIndex(b.id)).map((t) => ({
    target: t.name,
    family: t.config?.family ?? 'auto',
    probe:   t.config?.probe   ?? 1,
    timeout: t.config?.timeout ?? 0,
    payload: t.config?.payload ?? 56,
    maxhops: t.config?.maxhops ?? 30,
    raw:     t.config?.raw     ?? true,
    src:     t.config?.src     ?? '',
    protocol: t.config?.protocol ?? 'icmp',
    destPort: t.config?.destPort ?? 33434,
  }))

  // Native "Save As…" for exported files — routes through the real OS save
  // dialog (window.netpulse.saveFile, backed by @tauri-apps/plugin-dialog)
  // and writes with window.netpulse.writeFile (a plain std::fs::write on the
  // Rust side — see commands.rs), instead of the old `<a download>` trick
  // every export function here used to use. That trick was inherited from
  // the legacy browser/Electron builds: in a real browser it hands off to
  // the browser's own download manager, but inside a Tauri webview it has no
  // equivalent to hand off to — no dialog, no user-chosen name or location,
  // and on some platforms no file at all, silently. saveFile()/writeFile()
  // already existed in tauri-bridge.js and were simply never wired up to any
  // of the export functions below.
  // Returns true if a file was actually written, false if the user
  // cancelled the dialog (not an error — callers shouldn't show a modal for
  // that, only for a real write failure).
  const saveViaDialog = async (defaultName, filters, data) => {
    if (typeof window === 'undefined' || !window.netpulse?.saveFile) {
      throw new Error('Save dialog is unavailable — NetPulse is not running inside the Tauri app window.')
    }
    const path = await window.netpulse.saveFile(defaultName, filters)
    if (!path) return false // user cancelled — not an error
    const bytes = typeof data === 'string' ? new TextEncoder().encode(data) : data
    await window.netpulse.writeFile(path, bytes)
    return true
  }
  // Target/host names are free text (an IP, a hostname, whatever the user
  // typed) and end up in a suggested filename — strip characters that are
  // invalid (Windows) or awkward (everywhere) in a filename so the save
  // dialog never opens with a name it would itself reject.
  const sanitizeFilename = (s) => String(s || 'target').replace(/[\\/:*?"<>|\s]+/g, '_').slice(0, 80)

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
    try {
      await saveViaDialog('netpulse_targets.npulse', [{ name: 'NetPulse target list', extensions: ['npulse'] }], new Uint8Array(buf))
    } catch (e) {
      await showModal({ title: 'Save failed', message: String(e?.message || e) })
    }
  }

  const exportTargetsJson = async () => {
    try {
      await saveViaDialog('netpulse_targets.json', [{ name: 'JSON', extensions: ['json'] }],
        JSON.stringify({ version: 1, targets: targetListPayload() }, null, 2))
    } catch (e) {
      await showModal({ title: 'Save failed', message: String(e?.message || e) })
    }
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
      const importedOrder = [] // new ids, in the order this file lists them
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
            (c.src||'')       === (entry.src||'')              &&
            (c.protocol||'icmp') === (entry.protocol||'icmp')  &&
            String(c.destPort ?? 33434) === String(entry.destPort ?? 33434)
          )
        })
        if (existingDupe) { skipped++; continue }
        const q = new URLSearchParams({
          target: entry.target, family: entry.family ?? 'auto',
          probe: entry.probe ?? 1, trace: 30,
          timeout: entry.timeout ?? 0, payload: entry.payload ?? 56,
          maxhops: entry.maxhops ?? 30, raw: (entry.raw ?? true) ? '1' : '0',
          protocol: entry.protocol ?? 'icmp', destPort: entry.destPort ?? 33434,
        })
        if (entry.src) q.set('src', entry.src)
        const { id: newId } = await api(`/api/add?${q}`, { method: 'POST' })
        if (newId != null) importedOrder.push(newId)
        added++
      }
      await refreshState()
      // New ids get appended in the file's own order rather than whatever
      // order the engine happens to report them back in — the "keep
      // newly-added targets in sync" effect above already appends any
      // *unlisted* id to the end, but does so in engine-report order, which
      // is what silently scrambled a freshly-loaded list's arrangement.
      if (importedOrder.length) {
        setCustomOrder((prev) => [...prev.filter((id) => !importedOrder.includes(id)), ...importedOrder])
        setDashSortCol('custom')
      }
      const msg = [`Imported ${added} target${added !== 1 ? 's' : ''}.`, skipped ? `${skipped} skipped (already traced with same config).` : ''].filter(Boolean).join(' ')
      await showModal({ title: 'Import complete', message: msg, buttons: [{ label: 'OK', value: true, primary: true }] })
    } catch (e) {
      await showModal({ title: 'Import failed', message: `Import failed: ${e.message}`, buttons: [{ label: 'OK', value: true, primary: true }] })
    }
  }
  // ─────────────────────────────────────────────────────────────────────────

  // ── Global target controls (used by the menu bar and sidebar) ──────────────
  const allPaused = targets.length > 0 && targets.every((t) => t.paused)
  const pauseAll = (on) => Promise.all(targets.map((t) => ctrl(t.id, 'pause', `&on=${on ? 1 : 0}`))).then(refreshState).catch(() => {})
  const pauseOne = (t) => ctrl(t.id, 'pause', `&on=${t.paused ? 0 : 1}`).then(refreshState).catch(() => {})
  // Force-recheck: engine-side this immediately re-verifies every already-
  // known hop's identity (see core Session::force_recheck) — the same
  // non-destructive periodic re-probe the engine already runs to notice
  // route changes from a VPN toggle or network switch, just triggered on
  // demand instead of waiting out the timer. It does NOT clear hops/state,
  // so the UI only ever updates in place, never resets. Since there's no
  // reset snapshot for isDiscovering() to react to, track a brief local
  // "requested" pulse per target id so the button still gives immediate
  // visible feedback that a recheck is in flight.
  const [recheckRequested, setRecheckRequested] = useState(() => new Set())
  const forceRecheckOne = (t) => {
    setRecheckRequested((s) => new Set(s).add(t.id))
    setTimeout(() => setRecheckRequested((s) => { const n = new Set(s); n.delete(t.id); return n }), 2500)
    ctrl(t.id, 'recheck').then(refreshState).catch(() => {})
  }
  const removeAll = async () => {
    if (!targets.length) return
    const ok = await showModal({
      title: 'Remove all targets',
      message: `Remove all ${targets.length} target(s)?`,
      buttons: [
        { label: 'Cancel', value: false, kind: 'danger' },
        { label: 'Remove all', value: true, kind: 'danger' },
      ],
      blocking: true,
    })
    if (!ok) return
    Promise.all(targets.map((t) => ctrl(t.id, 'remove'))).then(() => { closeTarget(); refreshState() }).catch(() => {})
  }
  const quickAdd = (host) => { setForm((f) => ({ ...f, target: host })); setTimeout(() => addTargetHost(host), 0) }
  const [about, setAbout] = useState(false)
  const [aboutClosing, setAboutClosing] = useState(false)
  // Same delayed-unmount pattern as closeModal() (App.jsx, near showModal) —
  // see that function's own doc comment for why the close animation needs
  // this rather than just toggling a class. About has no return value to
  // resolve (it's not a promise-based prompt like showModal), so this is
  // simpler: just delay the actual setAbout(false) by the animation's
  // duration.
  const closeAbout = () => {
    setAboutClosing(true)
    setTimeout(() => { setAbout(false); setAboutClosing(false) }, 160)
  }
  // BUG FIX: this effect used to live near the top of the component,
  // ABOVE where `about` itself is declared (this line). Referencing a
  // variable in a useEffect DEPENDENCY ARRAY evaluates it immediately,
  // synchronously, at the point the useEffect(...) CALL itself executes
  // while App() runs top-to-bottom — unlike the effect BODY, which only
  // runs later. `about` is declared with `const`, so referencing it before
  // this line executes hits its temporal dead zone: "Cannot access 'about'
  // before initialization", thrown on every single render, permanently
  // (React's ErrorBoundary catches it, but nothing about state changes
  // afterward, so the whole app was stuck on that screen). Fixed by moving
  // this effect to right after the declaration it depends on, rather than
  // moving the declaration — far lower risk than relocating a `useState`
  // that other code earlier in the file might also assume is available.
  //
  // Escape closes the About dialog. Added alongside removing its
  // click-outside-to-dismiss behaviour: a deliberate keypress is a clear
  // intent to close, whereas a stray click on the backdrop is not.
  useEffect(() => {
    if (!about) return
    const onKey = (e) => { if (e.key === 'Escape') closeAbout() }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [about])
  const [appVersion, setAppVersion] = useState(null)
  useEffect(() => {
    if (!about || appVersion) return
    const np = (typeof window !== 'undefined') && window.netpulse
    if (np && np.getVersion) np.getVersion().then((v) => v && setAppVersion(v)).catch(() => {})
  }, [about, appVersion])
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
    // Same-round samples from different hops don't share an exact
    // timestamp (each hop's reply is timestamped when IT arrived, not when
    // the round started), so merging on the raw `ts` value almost always
    // puts each hop's point on its OWN row with every other series null
    // there. Rounding down to a bucket a fraction of the probe interval
    // wide instead groups same-round replies from different hops onto the
    // same row, while genuinely separate rounds (a full probe interval
    // apart) still land on separate rows — real loss/gaps still show as a
    // break in the line, they just aren't ARTIFICIALLY introduced between
    // every single point the moment a second series is added.
    const probeSecs = Math.max(0.05, sel.config?.probe || 1)
    const bucket = Math.max(probeSecs / 2, 0.05)
    const byTs = new Map()
    for (const h of shownHops) for (const [ts, rtt] of sel.series[String(h.hop)] || []) {
      const key = Math.round(ts / bucket)
      if (!byTs.has(key)) byTs.set(key, { ts })
      byTs.get(key)[`h${h.hop}`] = rtt
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
    // Overlay mode mixes several hops' values together, and different hops
    // have legitimately different baseline latencies (a near hop and the
    // destination aren't "outliers" of each other, they're just different
    // points on the path) — a single combined median across all of them
    // isn't a meaningful "typical value" the way it is for one hop's own
    // history. Skip clipping entirely here rather than compute a threshold
    // that would silently push an entire hop's normal data off the top of
    // the chart (confirmed: this is exactly what was happening — a
    // low near-hop-weighted median was clipping the destination hop's
    // completely ordinary ~30ms readings out of the visible range).
    if (!vals.length || !clipOutliers || overlay) return { yDomain: [0, 'auto'], clippedSpikes: 0 }
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
  }, [chartData, clipOutliers, overlay])


  // Captures the currently-shown latency chart as PNG bytes (a Promise
  // wrapper around the canvas/blob callback dance), used by exportPng below.
  const captureChartPng = () => new Promise((resolve, reject) => {
    // Recharts renders one small icon <svg class="recharts-surface"> per
    // legend entry, in addition to the actual chart's own <svg
    // class="recharts-surface"> — both share the same class, so a plain
    // querySelector('svg') can match a legend icon instead of the chart
    // once there are several legend entries (confirmed directly via
    // DevTools: with overlay-all-hops on, this was grabbing an svg with
    // aria-label="hop 1 legend icon", a 14×14 icon — which is exactly why
    // the exported PNG was a tiny dot instead of the chart). The main
    // plotting surface is always a direct child of .recharts-wrapper;
    // legend icons are nested deeper inside .recharts-legend-wrapper — the
    // direct-child combinator picks the right one unambiguously, rather
    // than relying on document order (which isn't guaranteed and is
    // exactly what broke here).
    const svg = chartRef.current?.querySelector(':scope .recharts-wrapper > svg.recharts-surface')
      || chartRef.current?.querySelector('svg')
    if (!svg) { resolve(null); return }
    // getBoundingClientRect(), not clientWidth/clientHeight — more reliable
    // across environments, and gives fractional precision recharts'
    // ResponsiveContainer actually rendered at.
    const rect = svg.getBoundingClientRect()
    const w = Math.max(1, Math.round(rect.width))
    const h = Math.max(1, Math.round(rect.height))
    // Clone rather than mutate the live chart, and explicitly bake width/
    // height IN AS XML ATTRIBUTES on the clone before serializing. The live
    // SVG's actual pixel size comes from ResponsiveContainer's own
    // ResizeObserver-based measurement of its parent container — that
    // sizing context doesn't necessarily survive being serialized out into
    // a standalone data: URI image with no parent to measure against,
    // which is a well-known cause of an SVG rasterizing at some fallback
    // size (sometimes 0×0) instead of the size it actually appeared at —
    // this is almost certainly why some environments produced a blank PNG.
    const clone = svg.cloneNode(true)
    clone.setAttribute('width', String(w))
    clone.setAttribute('height', String(h))
    if (!clone.getAttribute('viewBox')) clone.setAttribute('viewBox', `0 0 ${w} ${h}`)
    const xml = new XMLSerializer().serializeToString(clone)
    const img = new Image()
    img.onload = () => {
      // decode(), not just onload — onload can fire once the image's
      // intrinsic size is known, before the browser has necessarily
      // finished rasterizing complex nested SVG content (recharts' text
      // labels, tooltips, grid lines); decode() specifically guarantees
      // the image is fully ready to draw. This is the same pattern
      // html-to-image's own internals use for exactly this reason (see
      // exportHopsTablePng's comment above for what tracing that source
      // turned up).
      const ready = img.decode ? img.decode() : Promise.resolve()
      ready.then(() => {
        const c = document.createElement('canvas'); c.width = w * 2; c.height = h * 2
        const ctx = c.getContext('2d'); ctx.fillStyle = '#0e1116'; ctx.fillRect(0, 0, c.width, c.height); ctx.scale(2, 2); ctx.drawImage(img, 0, 0)
        c.toBlob(async (blob) => {
          if (!blob) { reject(new Error('Canvas produced no image data')); return }
          resolve(new Uint8Array(await blob.arrayBuffer()))
        }, 'image/png')
      }).catch(reject)
    }
    img.onerror = () => reject(new Error('Failed to rasterize the chart (the browser could not load the serialized SVG)'))
    img.src = 'data:image/svg+xml;base64,' + btoa(unescape(encodeURIComponent(xml)))
  })
  const exportPng = async () => {
    try {
      const bytes = await captureChartPng()
      if (!bytes) { await showModal({ title: 'Nothing to export', message: 'No chart is currently visible.' }); return }
      await saveViaDialog(`netpulse_graph_${sanitizeFilename(sel?.name)}.png`, [{ name: 'PNG image', extensions: ['png'] }], bytes)
    } catch (e) {
      await showModal({ title: 'Save failed', message: String(e?.message || e) })
    }
  }
  const exportHopsCsv = async () => {
    if (!sel) return
    const head = 'hop,address,hostname,asn,network,loss_pct,cur,avg,min,max,jitter,std,sent,recv,is_dest,alerting,stale_address,stale_last_seen_unix\n'
    const rows = sel.hops.map((h) => {
      const displayAddr = h.address !== '*' ? h.address : (h.stale_address || h.address)
      const a = asn[displayAddr] || {}
      return [h.hop, h.address, h.hostname, a.asn ? `AS${a.asn}` : '', (a.holder || '').replace(/,/g, ' '), h.loss.toFixed(2), h.cur ?? '', h.avg ?? '', h.min ?? '', h.max ?? '', (h.jitter ?? 0).toFixed(2), (h.std ?? 0).toFixed(2), h.sent, h.recv, h.is_dest, isAlerting(h), h.address === '*' ? (h.stale_address || '') : '', h.address === '*' ? (h.stale_since ?? '') : ''].join(',')
    }).join('\n')
    try {
      await saveViaDialog(`netpulse_hops_${sanitizeFilename(sel.name)}.csv`, [{ name: 'CSV', extensions: ['csv'] }],
        `# ${sel.name} ${sel.dest_ip}\n` + head + rows)
    } catch (e) {
      await showModal({ title: 'Save failed', message: String(e?.message || e) })
    }
  }
  // A picture of the hop table itself — the "traceroute report" — distinct
  // from exportPng above (which captures the latency-over-time chart, not
  // the table). html-to-image (not the more common html2canvas) is used
  // deliberately: html2canvas has a long-standing, still-unresolved bug
  // failing to parse oklch()/color-mix() (SheetJS-unrelated, separate
  // library — see github.com/niklasvh/html2canvas/issues/3269), and this
  // app's CSS uses color-mix() throughout its theme variables (Tailwind
  // v4's default palette). html-to-image sidesteps the whole problem by
  // rendering through an SVG foreignObject — the browser's own CSS engine
  // paints it, rather than the library re-implementing CSS color parsing —
  // so it handles this app's actual stylesheet correctly.
  // Decodes a data: URL directly to bytes (base64 → Uint8Array) without
  // fetch(). fetch() on a data: URL is a `connect-src` CSP check — this
  // app's CSP (connect-src 'self' ipc: http://ipc.localhost https:)
  // doesn't list `data:`, so fetch(dataUrl) is silently blocked by the
  // browser's own CSP enforcement (confirmed directly from a DevTools
  // console error, not inferred). atob() is a plain synchronous string
  // API with no network request involved, so no CSP category applies to
  // it — this is the correct way to turn a data: URL into bytes inside a
  // CSP'd app, not a workaround.
  const dataUrlToBytes = (dataUrl) => {
    const base64 = dataUrl.slice(dataUrl.indexOf(',') + 1)
    const binary = atob(base64)
    const bytes = new Uint8Array(binary.length)
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
    return bytes
  }
  const exportHopsTablePng = async () => {
    if (!sel) return
    if (!hopTableRef.current) {
      await showModal({ title: 'Nothing to export', message: 'The hop table isn’t currently visible.' })
      return
    }
    try {
      const { toPng } = await import('html-to-image')
      // skipFonts: true — this app bundles no custom web fonts (see
      // README.md's Styling section), so there's nothing for html-to-image
      // to embed here. Without this, toPng() still walks every stylesheet
      // on the page looking for @font-face rules to inline, including
      // reading .cssRules on stylesheets it didn't load — a known source of
      // synchronous SecurityErrors on cross-origin sheets in some
      // browser/webview combinations, for a feature this app never uses.
      //
      // The timeout below is defense in depth on top of that: whatever the
      // exact cause on a given machine (an older WebView2 runtime not
      // supporting a JS API the library calls, e.g. HTMLImageElement's
      // decode() — checked the library's own source, which calls that
      // inside a raw onload handler where a throw would leave the promise
      // permanently pending, neither resolved nor rejected), this makes
      // sure the button can never again look like it's just doing nothing
      // forever — after 12s it fails LOUDLY with a real error instead.
      const dataUrl = await Promise.race([
        toPng(hopTableRef.current, { backgroundColor: '#0e1116', pixelRatio: 2, skipFonts: true }),
        new Promise((_, reject) => setTimeout(() => reject(new Error('Timed out capturing the hop table as an image (this can happen on some Windows systems if WebView2 needs updating).')), 12000)),
      ])
      const bytes = dataUrlToBytes(dataUrl)
      await saveViaDialog(`netpulse_traceroute_${sanitizeFilename(sel.name)}.png`, [{ name: 'PNG image', extensions: ['png'] }], bytes)
    } catch (e) {
      await showModal({ title: 'Export failed', message: String(e?.message || e) })
    }
  }

  // Full-history export: every raw sample this target (or all targets) has
  // ever recorded, not just the current summary row per hop — see
  // Manager::export_target_full_csv's doc comment (manager.hpp) for why
  // this is a dedicated backend call reading ColdStore directly rather than
  // reusing the chart's already-downsampled series data. Long-format (one
  // row per sample, not per hop), so it opens straight into a pivot table.
  const exportTargetFullCsv = async () => {
    if (!sel || typeof window === 'undefined' || !window.netpulse?.exportTargetCsv) return
    try {
      const csv = await window.netpulse.exportTargetCsv(sel.id)
      if (!csv) { await showModal({ title: 'Nothing to export', message: 'No history recorded for this target yet.' }); return }
      await saveViaDialog(`netpulse_full_${sanitizeFilename(sel.name)}.csv`, [{ name: 'CSV', extensions: ['csv'] }], csv)
    } catch (e) {
      await showModal({ title: 'Export failed', message: String(e?.message || e) })
    }
  }
  const exportAllTargetsFullCsv = async () => {
    if (typeof window === 'undefined' || !window.netpulse?.exportAllTargetsCsv) return
    try {
      const csv = await window.netpulse.exportAllTargetsCsv()
      if (!csv) { await showModal({ title: 'Nothing to export', message: 'No targets with recorded history yet.' }); return }
      await saveViaDialog('netpulse_full_all_targets.csv', [{ name: 'CSV', extensions: ['csv'] }], csv)
    } catch (e) {
      await showModal({ title: 'Export failed', message: String(e?.message || e) })
    }
  }

  // Wraps a worker using the streaming start/append/finish protocol (see
  // workers/xlsxWorker.js) — used by both Excel exports below. append()
  // can be called once (single target) or many times (fleet export, once
  // per target, so progress can be reported and no single call/transfer
  // scales with fleet size) before finish(). Each append's CSV text is
  // encoded to bytes and TRANSFERRED (not passed as a plain string):
  // postMessage structured-clones a string argument, meaning a synchronous
  // COPY on the main thread before the worker ever sees it — for a large
  // chunk that copy alone costs real, blocking time even though the actual
  // parsing happens off-thread. A transferred ArrayBuffer hands over
  // ownership instead of copying — O(1) regardless of size.
  const createXlsxWorker = () => {
    const worker = new Worker(new URL('./workers/xlsxWorker.js', import.meta.url), { type: 'module' })
    const encoder = new TextEncoder()
    let pending = null
    worker.onmessage = (e) => {
      if (e.data.type === 'done' && pending) {
        const { resolve, reject } = pending; pending = null
        if (e.data.ok) resolve(e.data.buffer)
        else reject(new Error(e.data.error))
      }
    }
    worker.onerror = (err) => { if (pending) { pending.reject(new Error(err.message || 'Worker error')); pending = null } }
    return {
      start(sheets, historySheetName) { worker.postMessage({ type: 'start', sheets, historySheetName }) },
      append(csvText) {
        const bytes = encoder.encode(csvText)
        worker.postMessage({ type: 'append', bytes: bytes.buffer }, [bytes.buffer])
      },
      finish() {
        return new Promise((resolve, reject) => { pending = { resolve, reject }; worker.postMessage({ type: 'finish' }) })
      },
      terminate() { worker.terminate() },
    }
  }

  // Excel export — everything the CSV full-history export has, PLUS the
  // current summary and config context, organized as separate sheets in one
  // workbook instead of three separate files. Reuses the exact same backend
  // data (window.netpulse.exportTargetCsv) rather than a new export path.
  const exportTargetFullXlsx = async () => {
    if (!sel || typeof window === 'undefined' || !window.netpulse?.exportTargetCsv) return
    const xw = createXlsxWorker()
    try {
      const csv = await window.netpulse.exportTargetCsv(sel.id)
      if (!csv) { await showModal({ title: 'Nothing to export', message: 'No history recorded for this target yet.' }); return }

      const configRows = [
        ['Field', 'Value'],
        ['Target', sel.name],
        ['Destination IP', sel.dest_ip || ''],
        ['Family', familyLabel(sel.family, sel.config?.family)],
        ['Probe interval (s)', sel.config?.probe ?? ''],
        ['Timeout (s)', sel.config?.timeout === 0 ? 'auto' : (sel.config?.timeout ?? '')],
        ['Payload (bytes)', sel.config?.payload ?? ''],
        ['Max hops', sel.config?.maxhops ?? ''],
        ['Interface', sel.config?.src || 'auto'],
        ['Exported at', new Date().toLocaleString()],
      ]

      const summaryHead = ['hop', 'address', 'hostname', 'asn', 'network', 'loss_pct', 'cur', 'avg', 'min', 'max', 'jitter', 'std', 'sent', 'recv', 'is_dest', 'alerting']
      const summaryRows = sel.hops.map((h) => {
        const displayAddr = h.address !== '*' ? h.address : (h.stale_address || h.address)
        const a = asn[displayAddr] || {}
        return [h.hop, h.address, h.hostname || '', a.asn ? `AS${a.asn}` : '', a.holder || '', Number(h.loss.toFixed(2)), h.cur ?? '', h.avg ?? '', h.min ?? '', h.max ?? '', Number((h.jitter ?? 0).toFixed(2)), Number((h.std ?? 0).toFixed(2)), h.sent, h.recv, h.is_dest, isAlerting(h)]
      })

      xw.start([
        { name: 'Config', rows: configRows },
        { name: 'Hop Summary', rows: [summaryHead, ...summaryRows] },
      ], 'Full History')
      xw.append(csv)
      const buf = await xw.finish()
      await saveViaDialog(`netpulse_full_${sanitizeFilename(sel.name)}.xlsx`, [{ name: 'Excel workbook', extensions: ['xlsx'] }], new Uint8Array(buf))
    } catch (e) {
      await showModal({ title: 'Export failed', message: String(e?.message || e) })
    } finally {
      xw.terminate()
    }
  }

  // Dashboard-level Excel export — every target, every hop's current
  // summary, AND every target's full raw history, in one workbook.
  //
  // THIS is the one that scales with fleet size, and it's built to stay
  // responsive regardless: instead of one backend call for the whole fleet
  // (window.netpulse.exportAllTargetsCsv — the OLD approach), it reuses the
  // existing, already-small/bounded per-target exportTargetCsv(id) command
  // in a sequential loop, one target at a time. Three things fall out of
  // that on their own, without any backend changes:
  //   - each backend call, each Tauri IPC decode, and each transfer into
  //     the worker is bounded by ONE target's data, never the whole fleet's
  //     — so none of those steps individually scales with fleet size into
  //     a single freeze-sized chunk of work;
  //   - each `await` naturally yields back to the browser between targets,
  //     so the UI gets many small breathing gaps instead of one long
  //     uninterrupted block — this is the actual fix for the freeze, not
  //     just moving work to a worker;
  //   - real per-target progress becomes available for free, shown on the
  //     button itself via xlsxExportProgress (see TargetPanel.jsx).
  // Sequential, not parallel (Promise.all) on purpose — concurrent calls
  // would all contend for the same backend locks (Manager::listMtx_ plus
  // each target's own mutex) at once; one at a time is the safer default
  // at real fleet scale, and it's what makes the per-target progress UI
  // meaningful in the first place.
  const exportAllTargetsFullXlsx = async () => {
    if (typeof window === 'undefined' || !window.netpulse?.exportTargetCsv) return
    if (!targets.length) { await showModal({ title: 'Nothing to export', message: 'No targets yet.' }); return }

    const xw = createXlsxWorker()
    setXlsxExportProgress({ done: 0, total: targets.length })
    try {
      const overviewHead = ['id', 'name', 'dest_ip', 'family', 'paused', 'error', 'hop_count', 'probe_interval_s', 'timeout_s', 'payload_bytes', 'max_hops', 'interface']
      const overviewRows = targets.map((t) => [
        t.id, t.name, t.dest_ip || '', familyLabel(t.family, t.config?.family), !!t.paused, t.error || '',
        t.hops?.length || 0, t.config?.probe ?? '', t.config?.timeout ?? '', t.config?.payload ?? '', t.config?.maxhops ?? '', t.config?.src || 'auto',
      ])

      const hopsHead = ['target_id', 'target_name', 'hop', 'address', 'hostname', 'asn', 'network', 'loss_pct', 'cur', 'avg', 'min', 'max', 'jitter', 'std', 'sent', 'recv', 'is_dest']
      const hopsRows = []
      for (const t of targets) {
        for (const h of (t.hops || [])) {
          const displayAddr = h.address !== '*' ? h.address : (h.stale_address || h.address)
          const a = asn[displayAddr] || {}
          hopsRows.push([t.id, t.name, h.hop, h.address, h.hostname || '', a.asn ? `AS${a.asn}` : '', a.holder || '', Number(h.loss.toFixed(2)), h.cur ?? '', h.avg ?? '', h.min ?? '', h.max ?? '', Number((h.jitter ?? 0).toFixed(2)), Number((h.std ?? 0).toFixed(2)), h.sent, h.recv, h.is_dest])
        }
      }

      xw.start([
        { name: 'Targets Overview', rows: [overviewHead, ...overviewRows] },
        { name: 'All Hops Summary', rows: [hopsHead, ...hopsRows] },
      ], 'Full History')

      for (let i = 0; i < targets.length; i++) {
        const csv = await window.netpulse.exportTargetCsv(targets[i].id)
        if (csv) xw.append(csv)
        setXlsxExportProgress({ done: i + 1, total: targets.length })
      }

      const buf = await xw.finish()
      await saveViaDialog('netpulse_full_all_targets.xlsx', [{ name: 'Excel workbook', extensions: ['xlsx'] }], new Uint8Array(buf))
    } catch (e) {
      await showModal({ title: 'Export failed', message: String(e?.message || e) })
    } finally {
      xw.terminate()
      setXlsxExportProgress(null)
    }
  }

  const exportTargets = (json) => {
    if (json) download('netpulse_targets.json', JSON.stringify(targets.map((t) => ({ id: t.id, target: t.name, dest_ip: t.dest_ip })), null, 2), 'application/json')
    else download('netpulse_targets.csv', 'id,target,dest_ip\n' + targets.map((t) => `${t.id},${t.name},${t.dest_ip}`).join('\n'), 'text/csv')
  }

  const dest = sel ? sel.hops.find((h) => h.is_dest) : null
  const asnOf = (ip) => asn[ip] || null

  function CustomTooltip({ active, payload, label }) {
    if (!active || !payload || !payload.length || !sel) return null
    const time = timeFmt(label)
    return (
      <div className="chart-tooltip">
        <div className="chart-tooltip-header">{time}</div>
        <div className="chart-tooltip-body">
          {payload.map((p, idx) => {
            const hopNo = Number(String(p.dataKey || '').replace(/^h/, ''))
            const hop = sel.hops.find((h) => Number(h.hop) === hopNo)
            if (!hop) return null
            const info = asn[hop.address] || {}
            return (
              <div key={idx} className="chart-tooltip-row">
                <div className="ctr-left">
                  <div className="ctr-hop">Hop {hop.hop}</div>
                  <div className="ctr-host">{hop.hostname || hostCache[hop.address] || hop.address}</div>
                  <div className="ctr-meta">{info.asn ? `AS${info.asn}` : '—'}{info.holder ? ` · ${info.holder}` : ''}</div>
                </div>
                <div className="ctr-right">
                  <div className="ctr-rtt">{p.value == null ? '—' : `${p.value.toFixed(1)} ms`}</div>
                  <div className="ctr-statline">sent {hop.sent} · recv {hop.recv} · loss {hop.loss.toFixed(1)}%</div>
                  <div className="ctr-stats">med {fmt(hop.med)} · avg {fmt(hop.avg)} · min {fmt(hop.min)} · max {fmt(hop.max)} · jit {fmt(hop.jitter)}</div>
                </div>
              </div>
            )
          })}
        </div>
      </div>
    )
  }

  return (
    <div className="app">
      {/* BUG FIX: the Modal component above was DEFINED but never actually
          rendered anywhere — `<Modal` appeared zero times in this file. Since
          showModal() works by setting state and returning a Promise that only
          resolves when closeModal() runs, and closeModal() is only ever called
          by the Modal's own buttons, every one of the 27 showModal() call
          sites in this file returned a promise that could NEVER resolve. Any
          `await showModal(...)` therefore hung that function forever, silently:
          no dialog appeared, no exception was thrown, nothing reached the
          console. That is exactly the reported "selecting TCP/UDP shows no
          alert AND the Add button does nothing" — addTarget() was suspended
          mid-await, so it never got as far as adding anything. This is a
          pre-existing bug (present in the original upstream file too, not
          introduced by the protocol-prerequisite work built on top of it);
          rendering the component here is what makes every one of those call
          sites function as intended. */}
      {modal && (
        <Modal
          title={modal.title}
          message={modal.message}
          details={modal.details}
          buttons={modal.buttons}
          blocking={modal.blocking}
          closing={modalClosing}
          onClose={closeModal}
        />
      )}
      <MenuBar menus={[
        {
          label: 'File', items: [
            { label: 'Add target…', onClick: () => { document.querySelector('input.target')?.focus() }, hint: 'Focus the host/IP field' },
            { sep: true },
            { label: 'Save target list (.npulse)…', onClick: exportTargetList, disabled: !targets.length },
            { label: 'Export targets as JSON…', onClick: exportTargetsJson, disabled: !targets.length },
            { label: 'Load target list (.npulse)…', onClick: () => importInputRef.current?.click() },
            { sep: true },
            { label: 'Export hops CSV (selected)…', onClick: () => sel && exportHopsCsv && exportHopsCsv(), disabled: !sel },
            { label: 'Export FULL history CSV (selected target)…', onClick: exportTargetFullCsv, disabled: !sel, hint: 'Every recorded sample, all hops, all history' },
            { label: 'Export FULL history CSV (all targets)…', onClick: exportAllTargetsFullCsv, disabled: !targets.length, hint: 'Every recorded sample, every target' },
            { sep: true },
            { label: 'Export to Excel (selected target)…', onClick: exportTargetFullXlsx, disabled: !sel, hint: 'Config + hop summary + full history, one workbook' },
            { label: 'Export to Excel (all targets)…', onClick: exportAllTargetsFullXlsx, disabled: !targets.length, hint: 'Every target — overview + all hops + full history' },
          ],
        },
        {
          label: 'Targets', items: [
            { label: allPaused ? 'Resume all' : 'Pause all', onClick: () => pauseAll(!allPaused), disabled: !targets.length },
            { label: sel ? (sel.paused ? 'Resume selected' : 'Pause selected') : 'Pause selected', onClick: () => sel && pauseOne(sel), disabled: !sel },
            { sep: true },
            { label: 'Remove selected', onClick: () => sel && ctrl(sel.id, 'remove').then(() => { closeTarget(); refreshState() }), disabled: !sel },
            { label: 'Remove all…', onClick: removeAll, disabled: !targets.length },
            { sep: true },
            { label: 'Force recheck selected', onClick: () => sel && forceRecheckOne(sel), disabled: !sel },
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
            { label: 'Path / MTR (home)', checked: tab === 'path', onClick: () => setTab('path') },
            { label: 'Ping', checked: tab === 'ping', onClick: () => setTab('ping') },
            { label: 'DNS Lookup (forward/reverse)', checked: tab === 'dns', onClick: () => setTab('dns') },
            { label: 'Port Scanner', checked: tab === 'ports', onClick: () => setTab('ports') },
            { sep: true },
            { label: 'Quick trace 1.1.1.1 (Cloudflare)', onClick: () => { setTab('path'); addTargetHost('1.1.1.1') } },
            { sep: true },
            {
              // No environment variable needed — "Run as administrator"
              // starts a fresh elevated process that inherits none of the
              // launching terminal's environment, so NETPULSE_DEBUG=1 can't
              // be set for the elevated case at all. This writes to a file
              // instead, which works however the app was started.
              label: 'Diagnostic logging (writes a log file)',
              checked: debugLogging,
              onClick: async () => {
                const t = toolsApi()
                if (typeof t?.setDebugLogging !== 'function') {
                  await showModal({ title: 'Not available', message: 'This build does not support the diagnostic logging toggle.' })
                  return
                }
                const next = !debugLogging
                try {
                  const path = await t.setDebugLogging(next)
                  setDebugLogging(next)
                  const title = next ? 'Diagnostic logging enabled' : 'Diagnostic logging disabled'
                  // BUG FIX (round 1): this used to always go through the
                  // in-app showModal WITHOUT `blocking: true` — closeable by
                  // an accidental click outside it, same defect already
                  // found and fixed for the About dialog, just missed here.
                  //
                  // BUG FIX (round 2): the first fix switched this to a
                  // native OS dialog specifically to get the OS's own alert
                  // sound "for free" — but that meant giving up the app's
                  // own visual styling for a plain, out-of-place-looking
                  // native message box, which wasn't the actual ask. The
                  // sound and the visual styling are genuinely independent
                  // concerns: play_alert_sound() (fire-and-forget, sound
                  // only, no dialog shown at all) supplies the OS's real
                  // system alert sound, while showModal(blocking: true)
                  // keeps the app-styled, theme-consistent dialog — same
                  // "must be dismissed via its own button" protection as
                  // before, restored to looking like the rest of the app.
                  if (typeof t?.playAlertSound === 'function') t.playAlertSound('info').catch(() => {})
                  await showModal({
                    title,
                    message: next ? 'Reproduce the problem now, then send this file.' : 'Logging stopped. The existing file is kept.',
                    details: <div style={{ fontFamily: 'ui-monospace, monospace', wordBreak: 'break-all' }}>{path}</div>,
                    blocking: true,
                  })
                } catch (e) {
                  await showModal({ title: 'Could not change diagnostic logging', message: String(e && e.message || e) })
                }
              },
            },
            { label: 'Quick trace 8.8.8.8 (Google)', onClick: () => { setTab('path'); addTargetHost('8.8.8.8') } },
            { label: 'Quick trace 9.9.9.9 (Quad9)', onClick: () => { setTab('path'); addTargetHost('9.9.9.9') } },
            { sep: true },
            { label: 'Edit config (selected)…', onClick: () => { if (sel) { setEditForm({ probe: sel.config.probe, timeout: sel.config.timeout, payload: sel.config.payload, maxhops: sel.config.maxhops, family: sel.config.family, src: sel.config.src, protocol: sel.config.protocol || 'icmp', destPort: sel.config.destPort || 33434 }); setEditErrs({}); setEditing(true) } }, disabled: !sel },
          ],
        },
        {
          label: 'Help', items: [
            { label: 'About Net Pulse — Open Net Tools', onClick: () => setAbout(true) },
            { label: 'Check for Updates…', onClick: checkForUpdatesManually },
            { label: 'Project on GitHub', onClick: () => { try { window.open('https://github.com/ArchismanKarmakar/Net-Pulse-Open-Net-Tools', '_blank') } catch {} } },
          ],
        },
      ]} />
      {about && (
        // BUG FIX: this used to close on ANY click on the overlay. Clicking
        // anywhere outside the box — including a slightly-missed click aimed
        // at the box itself, or the link inside it — silently dismissed the
        // dialog. Removing the overlay's onClick means it now closes only via
        // its own Close button (or Escape, handled below), which is what a
        // dialog the user deliberately opened from a menu should do.
        <div className={'about-overlay' + (aboutClosing ? ' closing' : '')}>
          <div className={'about-box' + (aboutClosing ? ' closing' : '')}>
            <div className="modal-header" style={{ marginBottom: 6 }}>
              <h2 style={{ margin: 0 }}>Net Pulse — Open Net Tools</h2>
              <button className="modal-x" onClick={closeAbout} aria-label="Close" title="Close">✕</button>
            </div>
            <p className="about-version" style={{ margin: '0 0 10px' }}>
              {appVersion ? `v${appVersion}` : 'Version —'} <span style={{ opacity: 0.6, fontSize: 12 }}>(patch {PATCH_BUILD})</span>
            </p>
            <p style={{ margin: '0 0 10px', opacity: 0.75 }}>Cross-platform path-latency &amp; network diagnostics. Native C++ probe engine (via Rust/C++ FFI) + Tauri UI.</p>
            <p style={{ margin: '0 0 14px' }}>
              <a
                href="https://github.com/ArchismanKarmakar/Net-Pulse-Open-Net-Tools"
                target="_blank" rel="noreferrer"
                onClick={(e) => { e.preventDefault(); try { window.open('https://github.com/ArchismanKarmakar/Net-Pulse-Open-Net-Tools', '_blank') } catch {} }}
              >
                github.com/ArchismanKarmakar/Net-Pulse-Open-Net-Tools
              </a>
            </p>
            <p style={{ margin: '0 0 14px', opacity: 0.6, fontSize: 12 }}>
              AGPL-3.0 &middot; © Archisman Karmakar
            </p>
            <button className="primary" onClick={closeAbout}>Close</button>
          </div>
        </div>
      )}
      {updateInfo && updateInfo.available && !updateDismissed && (
        <div className="update-banner">
          <span>🔔 Net Pulse {updateInfo.version} is available.</span>
          {updateBusy ? (
            <span className="muted">
              {updateProgress && updateProgress.total
                ? `Downloading… ${Math.round((updateProgress.downloaded / updateProgress.total) * 100)}%`
                : 'Downloading…'}
            </span>
          ) : (
            <>
              <button className="primary" onClick={installUpdateNow}>Update &amp; Restart</button>
              <button onClick={() => setUpdateDismissed(true)}>Later</button>
            </>
          )}
        </div>
      )}
      <nav className="tabbar">
        {[['path', '🌐 Path / MTR'], ['ping', '📡 Ping'], ['dns', '🔎 DNS Lookup'], ['ports', '🔌 Port Scanner']].map(([id, label]) => (
          <button key={id} className={'tab' + (tab === id ? ' active' : '')} onClick={() => setTab(id)}>{label}</button>
        ))}
      </nav>
      {tab === 'ping' && <PingPage />}
      {tab === 'dns' && <DnsPage />}
      {tab === 'ports' && <PortScanPage />}
      {tab === 'path' && (<>
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
          <button className="primary" onClick={() => {
            // BUG FIX: addTarget() is async with no .catch() anywhere in its
            // call chain — React's onClick doesn't catch promise rejections
            // for you, so any uncaught exception inside it (this file's own
            // capability-check code included) became an "Unhandled promise
            // rejection" visible ONLY in DevTools console, and only if the
            // console's own level filter isn't hiding it (a live report's
            // own DevTools screenshot showed "4 hidden" messages — exactly
            // the kind of thing that can make a real error invisible even
            // with the console open). Surfacing it as an on-screen modal
            // means "+ Add does nothing" can never happen silently again —
            // worst case, the user sees exactly what broke.
            addTarget().catch((e) => {
              console.error('[NetPulse] addTarget() threw:', e)
              showModal({ title: 'Could not add target', message: String(e && e.message || e), details: <div>Open DevTools (F12 → Console) for the full error if this keeps happening.</div> })
            })
          }}>＋ Add</button>
          <label>Family<select value={form.family} onChange={(e) => setForm({ ...form, family: e.target.value })}>
            <option value="auto">Auto</option><option value="v4">IPv4</option><option value="v6">IPv6</option></select></label>
          <label>Protocol<select value={form.protocol} onChange={(e) => {
            const protocol = e.target.value
            const destPort = protocol === 'tcp' ? 443 : protocol === 'udp' ? 33434 : form.destPort
            setForm({ ...form, protocol, destPort })
            // Proactive: check the moment UDP/TCP is picked, before the host
            // is even typed or Add is pressed — the whole point is to warn
            // as early as possible rather than let someone fill in a target,
            // click Add, and only THEN find out it can't work. Fire-and-forget
            // (not awaited) since selecting an option shouldn't block the UI;
            // addTarget() re-checks authoritatively before actually adding
            // regardless of whether this finished or what it showed.
            // Same fix as the Add button's onClick — this fire-and-forget
            // call had no .catch() either, so any exception here became an
            // invisible unhandled promise rejection.
            if (protocol !== 'icmp') {
              checkProtocolPrerequisites(protocol, form.target, () => setForm((f) => (f.protocol === protocol ? { ...f, protocol: 'icmp' } : f)))
                .catch((err) => {
                  console.error('[NetPulse] checkProtocolPrerequisites() threw:', err)
                  showModal({ title: 'Protocol check failed', message: String(err && err.message || err), details: <div>Open DevTools (F12 → Console) for the full error if this keeps happening.</div> })
                })
            }
          }}>
            <option value="icmp">ICMP</option>
            <option value="udp">UDP</option>
            <option value="tcp">TCP</option>
          </select></label>
          {(form.protocol === 'udp' || form.protocol === 'tcp' || form.protocol === 'http') && (
            <label title={form.protocol === 'udp'
              ? "Destination port probed each hop — the classic traceroute base port (33434) is deliberately obscure so it's unlikely anything's actually listening"
              : form.protocol === 'http'
              ? "Plain HTTP port — 80 is standard. This actually sends a request and waits for a real HTTP response, not just a completed connection."
              : "TCP port to connect to — 443 (HTTPS) or 80 (HTTP) are usually open on real servers; a closed port still proves reachability via the RST it sends back"}>
              Port<input type="number" min="1" max="65535" step="1" value={form.destPort} onChange={(e) => setForm({ ...form, destPort: e.target.value })} />
            </label>
          )}
          <label title="Seconds between probes per hop (0.1–3600)">Probe<input type="number" min="0.1" max="3600" step="0.1" value={form.probe} onChange={(e) => setForm({ ...form, probe: e.target.value })} /></label>
          <label title="Route re-discovery interval, seconds (1–86400)">Trace<input type="number" min="1" max="86400" step="1" value={form.trace} onChange={(e) => setForm({ ...form, trace: e.target.value })} /></label>
          <label title="Per-probe timeout, seconds; 0 = auto (0–3600)">Timeout<input type="number" min="0" max="3600" step="0.1" value={form.timeout} onChange={(e) => setForm({ ...form, timeout: e.target.value })} /></label>
          <label title="ICMP payload bytes (0–65500; &gt;~1472 is fragmented)">Payload<input type="number" min="0" max="65500" step="1" value={form.payload} onChange={(e) => setForm({ ...form, payload: e.target.value })} /></label>
          <label title="Maximum hops / TTL (1–255)">Hops<input type="number" min="1" max="255" step="1" value={form.maxhops} onChange={(e) => setForm({ ...form, maxhops: e.target.value })} /></label>
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
        <TargetPanel
          compact={!!sel}
          sidebarWidth={sidebarWidth}
          isResizing={resizing === 'sidebar'}
          startDrag={startDrag}
          panelRef={sidebarElRef}
          targets={targets}
          rows={dashboardRows}
          destSummary={destSummary}
          pathSummary={pathSummary}
          statusFilter={statusFilter}
          toggleStatusFilter={toggleStatusFilter}
          targetSearch={targetSearch}
          setTargetSearch={setTargetSearch}
          dashSortCol={dashSortCol}
          dashSortDir={dashSortDir}
          toggleDashSort={toggleDashSort}
          selectDashSort={selectDashSort}
          onQuickAdd={quickAdd}
          onOpenTool={setTab}
          onReorder={handleReorder}
          moveTarget={moveTarget}
          forceRecheckOne={forceRecheckOne}
          pauseOne={pauseOne}
          removeTarget={removeTarget}
          openTarget={selectTarget}
          targetState={targetState}
          destLamp={destLamp}
          pathLamp={pathLamp}
          destHopOf={destHopOf}
          familyLabel={familyLabel}
          fmt={fmt}
          STATE_LABEL={STATE_LABEL}
          view={view}
          alertMs={alerts.ms}
          sel={sel}
          allPaused={allPaused}
          pauseAll={pauseAll}
          exportTargetList={exportTargetList}
          exportTargetsJson={exportTargetsJson}
          onLoadListClick={() => importInputRef.current?.click()}
          exportAllTargetsFullXlsx={exportAllTargetsFullXlsx}
          xlsxExportProgress={xlsxExportProgress}
        />
        <input
          ref={importInputRef}
          type="file"
          accept=".npulse"
          style={{ display: 'none' }}
          onChange={(e) => { if (e.target.files[0]) importTargetList(e.target.files[0]); e.target.value = '' }}
        />

        <main>
          {sel && (
            <>
              {(() => {
                const stFull = targetState(sel)
                const STATUS_TAG = { discovering: 'DISCOVERING', ok: 'OK', okloss: 'MINOR LOSS', warn: 'WARN', bad: 'BAD', down: 'DOWN' }
                return (
              <div className={'statusbar st-' + stFull}>
                <button className="btn-close-target" title="Back to the dashboard (target keeps running, nothing is deleted)" onClick={closeTarget}>← Dashboard</button>
                <span className="divider" />
                <span className={'status-pill st-' + stFull}>{STATUS_TAG[stFull] || stFull}</span>
                <b>{sel.name}</b> → {sel.dest_ip || 'resolving…'} <span className="badge">{familyLabel(sel.family, sel.config?.family)}</span>
                {sel.config?.protocol && sel.config.protocol !== 'icmp' && (
                  <span className="badge tc-proto-badge" title={`Probing over ${sel.config.protocol.toUpperCase()}, port ${sel.config.destPort}`}>
                    {sel.config.protocol.toUpperCase()}:{sel.config.destPort}
                  </span>
                )}
                {' '}· {sel.hops.length} hops
                <button className="btn-recheck" disabled={recheckRequested.has(sel.id)} title="Re-verify this target's route now, without resetting anything" onClick={() => forceRecheckOne(sel)}>↻ Force recheck</button>
                {(isDiscovering(sel) || recheckRequested.has(sel.id)) && !sel.error && (
                  <span className="badge inline-flex items-center align-middle" title="Discovering the route: resolving the destination and probing each hop. Each hop's first few real replies are used only to establish its address/route and aren't shown as stats yet, so an unrepresentative first reading isn't displayed as if it were typical. Clears per-hop as soon as real data is available.">
                    <span className="spinner sm" style={{ marginRight: 5 }} />
                    {recheckRequested.has(sel.id) && !isDiscovering(sel) ? 'recheck requested…' : sel.dest_ip ? 'discovering route…' : `resolving ${sel.name}…`}
                  </span>
                )}
                {(() => { const p = sel.hops.filter((h) => ['loss', 'warn', 'bad', 'down'].includes(hopStatus(h))); return p.length > 0 && <span className="err"> ⚠ {p.length} hop{p.length > 1 ? 's' : ''} with loss/latency</span> })()}
                {(() => {
                  const h1 = sel.hops[0]
                  const firewallLikely = !sel.error && h1 && h1.sent >= 5 && h1.recv === 0
                  return firewallLikely && (
                    <span className="err" title="Hop 1 is your own router — it should reply almost instantly. 100% loss specifically here (not deeper hops) almost always means a local firewall or antivirus is silently dropping ICMP packets, not a real network problem.">
                      {' '}⚠ No replies from hop 1 (your router) — likely a firewall/antivirus blocking ICMP on this PC, not a network issue.
                      Try allowing "Net Pulse" through Windows Firewall (or reinstall — newer installers add this automatically).
                    </span>
                  )
                })()}
                {sel.error && <span className="err"> ⚠ {sel.error}</span>}
                {(() => {
                  // No fatal error, engine has retried several times via the
                  // total-silence self-heal (see silenceRebuildCount's doc
                  // comment in session.hpp), and it's still been a while
                  // since ANY hop (including a direct ping to the
                  // destination itself) got a real reply — the engine isn't
                  // stuck or stopped, it's genuinely not getting anything
                  // back. That combination — silent to THIS app's raw ICMP
                  // while `ping <host>` from a terminal (a different code
                  // path — the OS's own ICMP helper, not a raw socket) still
                  // works — is the signature of something outside the app
                  // (firewall/AV/EDR, or occasionally an ISP/security
                  // appliance) specifically blocking this process's raw
                  // ICMP to that one destination, not a real outage.
                  if (sel.error || stFull === 'discovering') return null
                  const rebuilds = sel.silenceRebuildCount || 0
                  const silentFor = sel.lastAnyReplyAt ? (Date.now() / 1000 - sel.lastAnyReplyAt) : null
                  if (rebuilds < 3 || silentFor == null || silentFor < 60) return null
                  return (
                    <span className="err" title="The app has been automatically rebuilding this target's connection and retrying, not sitting idle — it just isn't getting any reply back, from any hop, including the destination itself.">
                      {' '}⚠ {rebuilds} automatic reconnect attempts, still no reply in {agoFmt(sel.lastAnyReplyAt)} — if a plain `ping {sel.name}` from a terminal works right now, this usually means a firewall/antivirus/security tool is blocking THIS app's ICMP specifically, not a real outage.
                    </span>
                  )
                })()}
                {sel.loopWarning && (
                  <span className="err warn-loop" title="This is advisory, not fatal — every hop below is still measured normally. See the highlighted rows and the banner above the table for the two hops involved.">
                    {' '}⚠ {sel.loopWarning}
                  </span>
                )}
                {sel.protocolHint && (
                  <span className="err warn-loop" title="Advisory, not fatal — the hops already discovered are still valid; this only explains why the destination itself may never confirm.">
                    {' '}⚠ {sel.protocolHint}
                  </span>
                )}
                <div className="spacer" />
                {dest && <span className="rtt">RTT <b>{fmt(dest.med ?? dest.avg)}</b> ms <span className="rttsub">(median · avg {fmt(dest.avg)} · cur {dest.cur == null ? '*' : fmt(dest.cur)})</span></span>}
                <button className="icon-btn" onClick={exportPng} title="Graph PNG — a picture of the latency chart" aria-label="Export graph as PNG"><IconImage /></button>
                <button className="icon-btn" onClick={exportHopsCsv} title="Hops CSV — current per-hop summary" aria-label="Export hops as CSV"><IconCsv /></button>
                <button className="icon-btn" onClick={exportHopsTablePng} title="Traceroute PNG — a picture of the hop table itself, not the latency chart" aria-label="Export hop table as PNG"><IconRoute /></button>
                <button className="icon-btn" onClick={exportTargetFullCsv} title="Full CSV — every recorded sample for this target, all hops, all history — not just the current summary row" aria-label="Export full history as CSV"><IconBackupTable /></button>
                <button className="icon-btn" onClick={exportTargetFullXlsx} title="Excel — same as Full CSV, plus config and current hop summary, as one workbook with separate sheets" aria-label="Export to Excel"><IconTableChart /></button>
                <span className="divider" />
                <button className="btn-close-x" title="Remove this target" aria-label="Remove target" onClick={() => removeTarget(sel.id, true)}>✕</button>
              </div>
                )
              })()}

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
                  {/* BUG FIX: this used to show a single "local port" value
                      here, sampled from whichever one hop happened to be
                      picked at render time. That's structurally misleading
                      two different ways: for TCP the local port genuinely
                      varies per hop by design (a fresh socket per probe),
                      so any single value shown here is only ever true for
                      ONE hop, not "the" session; for UDP it's normally one
                      stable value for the whole session, but can go stale
                      if a silence-rebuild ever reassigns the persistent
                      socket a new port — this summary had no way to know
                      that had happened. The per-hop tooltip in the table
                      below doesn't have either problem: it reads directly
                      from that specific hop's own latest recorded value,
                      so it's always honest about exactly what it's
                      describing. Removed here; kept there, and in the Ping
                      tab and debug logs, where it's tied to one real,
                      individual request rather than summarized. */}
                  <div className="spacer" />
                  {!editing
                    ? <button onClick={() => { setEditForm({ probe: sel.config.probe, timeout: sel.config.timeout, payload: sel.config.payload, maxhops: sel.config.maxhops, family: sel.config.family, src: sel.config.src, protocol: sel.config.protocol || 'icmp', destPort: sel.config.destPort || 33434 }); setEditing(true) }}>⚙ Edit config</button>
                    : <button onClick={() => setEditing(false)}>✕ Close</button>}
                </div>
              )}

              {editing && editForm && (
                <div className="editpanel">
                  <label>Probe (s)<input type="number" step="0.1" min="0.1" value={editForm.probe} onChange={(e) => setEditForm({ ...editForm, probe: e.target.value })} /></label>
                  <label>Timeout (s, 0=auto)<input type="number" step="0.1" min="0" value={editForm.timeout} onChange={(e) => setEditForm({ ...editForm, timeout: e.target.value })} /></label>
                  <label title="ICMP payload bytes (0–65500)">Payload<input type="number" min="0" max="65500" step="1" value={editForm.payload} onChange={(e) => setEditForm({ ...editForm, payload: e.target.value })} /></label>
                  <label title="Maximum hops / TTL (1–255)">Max hops<input type="number" min="1" max="255" step="1" value={editForm.maxhops} onChange={(e) => setEditForm({ ...editForm, maxhops: e.target.value })} /></label>
                  <label>Family
                    <select value={editForm.family} onChange={(e) => setEditForm({ ...editForm, family: e.target.value })}>
                      <option value="auto">Auto</option><option value="v4">IPv4</option><option value="v6">IPv6</option>
                    </select>
                  </label>
                  <label title="Protocol can't be changed on a running trace — Session::run_tcp()/run_udp() only read it once at start (Protocol is set when a target is added), so a live in-place change wouldn't actually take effect. Fixed field.">
                    Protocol <b style={{ textTransform: 'uppercase' }}>{editForm.protocol || 'icmp'}</b>
                  </label>
                  {(editForm.protocol === 'udp' || editForm.protocol === 'tcp' || editForm.protocol === 'http') && (
                    <label title="Remote port. Unlike the other fields on this panel, this genuinely needs a fresh restart of the trace to take effect — see 'Apply' below.">
                      Remote port<input type="number" min="1" max="65535" step="1"
                        value={editForm.destPort ?? (editForm.protocol === 'tcp' ? 443 : editForm.protocol === 'http' ? 80 : 33434)}
                        onChange={(e) => setEditForm({ ...editForm, destPort: e.target.value })} />
                    </label>
                  )}
                  <label>Interface
                    <select value={editForm.src} onChange={(e) => setEditForm({ ...editForm, src: e.target.value })}>
                      <option value="">Auto (default route)</option>
                      {(() => {
                        const effectiveFamily = editForm.family === 'auto' && sel ? sel.family : editForm.family
                        return interfaces.filter((i) => effectiveFamily === 'auto' || (effectiveFamily === 'v6') === i.v6)
                          .map((i, k) => <option key={k} value={i.address}>{i.name} — {i.address}</option>)
                      })()}
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
                  <div style={{ fontSize: 40, lineHeight: 1 }}>⚠️</div>
                  <div className="loading-text" style={{ color: 'var(--danger)', fontSize: 18, fontWeight: 600, marginTop: 10 }}>
                    {sel.error}
                  </div>
                  {/needs? admin|need admin\/root/i.test(sel.error) && (
                    <div className="loading-sub" style={{ maxWidth: 480, textAlign: 'center', marginTop: 6 }}>
                      ICMP probing needs Administrator rights on Windows (root on macOS/Linux).
                      Relaunch Net Pulse as Administrator to fix this.
                    </div>
                  )}
                </div>
              ) : (
                <>
                  {sel.loopWarning && (
                    <div className="loop-banner" role="status">
                      <span className="loop-banner-icon">⚠</span>
                      <span className="loop-banner-text">
                        {sel.loopWarning} — every hop below is still being measured normally; this only flags that hop{' '}
                        <b>{sel.loopHop}</b> and hop <b>{sel.loopDupAt}</b> appear to share a physical path. Both rows are
                        highlighted below.
                      </span>
                      {sel.loopHop != null && (
                        <button className="loop-banner-jump" onClick={() => setSelHop(sel.loopHop)}>Jump to hop {sel.loopHop}</button>
                      )}
                      {sel.loopDupAt != null && (
                        <button className="loop-banner-jump" onClick={() => setSelHop(sel.loopDupAt)}>Jump to hop {sel.loopDupAt}</button>
                      )}
                    </div>
                  )}
                  <div className="tablewrap" ref={tableWrapElRef} style={{ maxHeight: `${tablePct}%` }}>
                    <table ref={hopTableRef}>
                      <thead><tr>
                        <th>Hop</th><th>PL%</th><th>IP</th><th>Host</th><th>ASN</th><th>Network</th>
                        <th>Sent</th><th>Recv</th><th>Loss%</th><th>Cur</th><th title="Median — stays stable through a single spike, unlike Avg/Max">Med</th><th>Avg</th><th>Min</th><th>Max</th><th>Jitter</th>
                        {view.trend && <th>Trend</th>}
                        {view.latency && <th className="lathead"><span>Latency Graph</span><span className="scalemax">{scaleMax} ms</span></th>}
                      </tr></thead>
                      <tbody>
                        {sel.hops.map((h, i) => {
                          const displayAddr = h.address !== '*' ? h.address : (h.stale_address || h.address)
                          const a = asnOf(displayAddr)
                          const prev = i > 0 ? asnOf(sel.hops[i - 1].address) : null
                          const boundary = a && prev && a.asn && prev.asn && a.asn !== prev.asn
                          const prevAvg = i > 0 ? sel.hops[i - 1].avg : null
                          const nextAvg = i < sel.hops.length - 1 ? sel.hops[i + 1].avg : null
                          const hopPaused = (sel.config?.pausedHops || []).includes(h.hop)
                          // Global/per-target pause (sel.paused) and per-hop pause
                          // (pausedHops) are independent backend mechanisms — pausing
                          // the whole target stops ALL probing regardless of which
                          // hops are individually paused, so a hop's pause button
                          // must reflect the EFFECTIVE state (paused for either
                          // reason), not just its own pausedHops membership. Without
                          // this, toggling an individual hop while the target is
                          // globally paused looked broken: the click did mutate
                          // pausedHops, but nothing visibly changed (nothing was being
                          // probed either way), and the icon didn't match reality.
                          const targetPaused = !!sel.paused
                          const effPaused = targetPaused || hopPaused
                          const isLoopHop = sel.loopHop != null && h.hop === sel.loopHop
                          const isLoopDup = sel.loopDupAt != null && h.hop === sel.loopDupAt
                          return (
                            <tr key={h.hop} className={(h.hop === selHop ? 'selrow' : '') + (effPaused ? ' hoppaused' : '') + ((isLoopHop || isLoopDup) ? ' looprow' : '')} onClick={() => setSelHop(h.hop)}>
                              <td>
                                <button className="hop-pause" disabled={targetPaused}
                                  title={targetPaused ? 'Target is paused — resume the target to control individual hops' : (hopPaused ? 'Resume probing this hop' : 'Pause probing this hop (reduce load)')}
                                  onClick={(e) => { e.stopPropagation(); if (!targetPaused) toggleHopPause(h.hop) }}>{effPaused ? '▶' : '⏸'}</button>
                                <span className={'hopdot st-' + hopStatus(h)} title={hopStatus(h)} />{h.hop}{h.is_dest ? ' ◀' : ''}
                                {form.protocol === 'tcp' && tcpPortState(h) && (
                                  <span
                                    className={'port-state ps-' + tcpPortState(h)}
                                    title={
                                      tcpPortState(h) === 'open' ? `Port ${sel.config?.destPort ?? ''} is open — a SYN-ACK was received.`
                                      : tcpPortState(h) === 'closed' ? `Port ${sel.config?.destPort ?? ''} is closed — the host replied with RST. Still reachable.`
                                      : 'No reply of any kind within the timeout — filtered (firewalled or dropped), or simply not answering.'
                                    }>
                                    {tcpPortState(h)}
                                  </span>
                                )}
                                {(isLoopHop || isLoopDup) && (
                                  <span className="loop-badge" title={isLoopHop
                                    ? `Repeats the address seen at hop ${sel.loopDupAt}`
                                    : `Its address reappears at hop ${sel.loopHop}`}>⟲</span>
                                )}
                              </td>
                              <td><div className="plbar"><span style={{ width: `${Math.min(100, h.loss)}%` }} /></div><i className={h.loss > 0 ? 'loss' : ''}>{h.loss.toFixed(0)}</i></td>
                              <td className={'mono' + (h.address === '*' && h.stale_address ? ' stale-cell' : '')}
                                  title={h.local_port ? `Local (source) port this hop's most recent probe used: ${h.local_port}` : undefined}>
                                {h.address !== '*' ? h.address : (h.stale_address
                                  ? <span title={`Last confirmed ${h.stale_since ? new Date(h.stale_since * 1000).toLocaleString() : 'a while ago'} — this hop hasn't answered since, so this address is no longer being measured, just remembered.${h.stale_seen_elsewhere_at ? ' It is still responding to at least one other target right now — likely a per-flow routing difference (ECMP), not that this device is down.' : ''}`}>
                                      {h.stale_address}<span className="stale-tag"> (stale, {agoFmt(h.stale_since)})</span>
                                      {h.stale_seen_elsewhere_at && <span className="stale-tag stale-elsewhere"> · live via another target</span>}
                                    </span>
                                  : '*')}
                              </td>
                              <td className={'host' + (h.address === '*' && h.stale_address ? ' stale-cell' : '')} title={hostOf(h)}>{hostOf(h)}</td>
                              <td className={'asn' + (boundary ? ' boundary' : '') + (h.address === '*' && h.stale_address ? ' stale-cell' : '')} onClick={(e) => { e.stopPropagation(); openDetail(displayAddr) }}>
                                {a ? (a.loading ? '…' : a.asn ? `AS${a.asn}` : '—') : (bgp.isPublicIp(displayAddr) ? '…' : 'priv')}
                              </td>
                              <td className={'net' + (h.address === '*' && h.stale_address ? ' stale-cell' : '')} title={a && a.holder} onClick={(e) => { e.stopPropagation(); openDetail(displayAddr) }}>{a && a.holder ? a.holder : ''}</td>
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
                        <Tooltip content={<CustomTooltip />} labelFormatter={timeFmt} contentStyle={{ background: "var(--panel)", border: "1px solid var(--border)", borderRadius: 6, color: "var(--text)" }} labelStyle={{ color: "var(--muted)" }} itemStyle={{ color: "var(--text)" }} />
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

        <BgpDrawer detail={detail} onClose={() => setDetail(null)} />
      </div>
      </>)}
    </div>
  )
}
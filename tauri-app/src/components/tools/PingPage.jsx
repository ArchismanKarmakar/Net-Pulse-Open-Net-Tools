import React from 'react'
import { toolsApi } from '../../lib/api'
import { validatePingCfg } from '../../lib/constants'
import ToolUnavailable from './ToolUnavailable'

// Native ping — ICMP echo or UDP (a Port-Unreachable from the destination
// itself, the closest UDP analog to a successful echo — see the backend's
// PingProtocol::Udp doc comment, ping_run.hpp, for the full reasoning),
// both via the same pooled-socket/shared-dispatcher engine the main
// multi-target monitor uses (PingRun), not the OS `ping` binary. Each
// result arrives from the backend already structured (seq/ok/rtt_ms/from/
// note) — this renders and computes stats directly from those fields;
// nothing here regex-matches text the way the old OS-process version had
// to.
export default function PingPage() {
  const [host, setHost] = React.useState('')
  const [opts, setOpts] = React.useState({
    protocol: 'icmp', destPort: 33434,
    count: 10, size: 56, timeout: 2000, ttl: '', interval: 1, family: 'auto', continuous: false,
  })
  const [lines, setLines] = React.useState([])
  const [running, setRunning] = React.useState(false)
  const [cmd, setCmd] = React.useState('')
  const idRef = React.useRef(null)
  const preRef = React.useRef(null)
  const set = (k, v) => setOpts((o) => ({ ...o, [k]: v }))
  const isUdp = opts.protocol === 'udp'
  const isTcp = opts.protocol === 'tcp'
  const usesPort = isUdp || isTcp

  // Capability check for the currently-selected protocol — re-fetched every
  // time the protocol changes to udp/tcp, not cached from page load, since
  // elevation/Npcap can change while this page stays open (e.g. the user
  // clicks "Restart as Administrator" and the app relaunches). null means
  // "unknown/still checking or ICMP selected" (no gate); an object means the
  // check completed. See App.jsx's checkProtocolPrerequisites for the
  // Path/MTR equivalent of this same logic — kept separate rather than
  // shared because this page has no access to App's modal system (a
  // different, sibling React tree), so this renders an inline alert instead
  // of a blocking dialog.
  const [caps, setCaps] = React.useState(null)
  const [relaunching, setRelaunching] = React.useState(false)
  React.useEffect(() => {
    if (!usesPort) { setCaps(null); return }
    let cancelled = false
    ;(async () => {
      let c = null
      const capsFn = toolsApi()?.capabilities
      if (typeof capsFn !== 'function') {
        // BUG FIX: optional chaining on a missing method (the old
        // `toolsApi()?.capabilities?.()`) throws NOTHING — it just silently
        // evaluates to undefined, so a stale build missing this bridge
        // function left zero trace anywhere. See App.jsx's matching fix
        // (checkProtocolPrerequisites) for the fuller explanation; same
        // root cause, same fix here.
        console.error('[NetPulse] toolsApi().capabilities is not a function — this build\'s frontend bridge does not have the capability probe.', toolsApi())
      } else {
        try { c = await capsFn() }
        catch (e) { console.error('[NetPulse] capabilities() check failed for the Ping tab:', e) }
      }
      if (!cancelled) setCaps(c)
    })()
    return () => { cancelled = true }
  }, [opts.protocol, usesPort])

  const needsElevation = usesPort && caps && !caps.elevated
  const needsCapture = isTcp && caps && caps.platform === 'windows' && !caps.capture
  const blocked = needsElevation // TCP without Npcap still measures the destination fine, so that alone never blocks — see the inline note below
  const restartElevated = async () => {
    setRelaunching(true)
    try { await toolsApi().relaunchElevated() }
    catch (e) {
      setRelaunching(false)
      setLines([{ seq: 0, ok: false, rtt_ms: null, from: '', note: `Could not restart elevated: ${String(e && e.message || e)}` }])
    }
    // On success the whole app relaunches — nothing more to do here.
  }

  React.useEffect(() => {
    const t = toolsApi(); if (!t) return
    const off1 = t.onPingLine((result) => { if (result.id === idRef.current) setLines((L) => [...L, result]) })
    const off2 = t.onPingDone(({ id }) => { if (id === idRef.current) setRunning(false) })
    return () => { off1 && off1(); off2 && off2() }
  }, [])
  React.useEffect(() => { if (preRef.current) preRef.current.scrollTop = preRef.current.scrollHeight }, [lines])

  const start = async () => {
    const t = toolsApi(); if (!t) return
    if (!host.trim()) { setLines([{ seq: 0, ok: false, rtt_ms: null, from: '', note: 'Enter a host or IP first.' }]); return }
    if (blocked) return // the inline alert below is already showing why and what to do about it
    // BUG FIX/FEATURE: the number inputs below only ever had HTML5 min/max
    // attributes, which are soft hints — they show a red outline but do NOT
    // stop the browser from reading and using an out-of-range value typed
    // directly (or pasted) into the field. There was no actual check here
    // before starting, matching the same gap Edit Config had (now fixed
    // with validateCfg there) — extended here with the Ping tab's own
    // limits, since its fields don't match MTR's (count/size/timeout-ms/
    // interval-s have no equivalent there). PingPage has no in-app modal
    // system of its own (that's a closure inside App.jsx, a separate
    // component tree), so this uses the native OS dialog — same sound
    // mechanism already used for the protocol-prerequisite prompts.
    const cfgErrs = validatePingCfg({ ...opts, destPort: usesPort ? opts.destPort : undefined })
    if (Object.keys(cfgErrs).length) {
      if (typeof t?.playAlertSound === 'function') t.playAlertSound('warning').catch(() => {})
      const msg = 'Fix these before starting:\n\n' + Object.values(cfgErrs).map((m) => '• ' + m).join('\n')
      if (typeof t?.nativeMessage === 'function') {
        await t.nativeMessage(msg, { title: 'Invalid ping settings', kind: 'warning' })
      } else {
        setLines([{ seq: 0, ok: false, rtt_ms: null, from: '', note: msg }])
      }
      return
    }
    setLines([]); setRunning(true)
    const payload = { count: opts.count, size: opts.size, timeout: opts.timeout, interval: opts.interval, family: opts.family, continuous: opts.continuous, protocol: opts.protocol }
    if (usesPort) payload.destPort = opts.destPort
    if (!usesPort && opts.ttl !== '' && opts.ttl != null) payload.ttl = opts.ttl // TTL is ICMP-only — a UDP ping always uses the full path (255), see the backend's PingProtocol::Udp doc comment
    const res = await t.pingStart(host.trim(), payload)
    if (!res || res.error) {
      setLines([{ seq: 0, ok: false, rtt_ms: null, from: '', note: (res && res.error) || 'Failed to start ping' }])
      setRunning(false)
      return
    }
    idRef.current = res.id; setCmd(res.cmd || '')
  }
  const stop = async () => { const t = toolsApi(); if (t && idRef.current) await t.pingStop(idRef.current); setRunning(false) }

  // Stats computed directly from the structured results — no text to parse.
  const stats = React.useMemo(() => {
    const real = lines.filter((l) => l.seq > 0) // seq 0 is a synthetic error/status line, not a probe result
    if (!real.length) return null
    const rtts = real.filter((l) => l.ok && l.rtt_ms != null).map((l) => l.rtt_ms)
    const replies = rtts.length
    const sent = real.length
    const timeouts = sent - replies
    const min = rtts.length ? Math.min(...rtts) : null
    const max = rtts.length ? Math.max(...rtts) : null
    const avg = rtts.length ? rtts.reduce((a, b) => a + b, 0) / rtts.length : null
    const jit = rtts.length > 1 ? rtts.slice(1).reduce((a, b, i) => a + Math.abs(b - rtts[i]), 0) / (rtts.length - 1) : 0
    const loss = sent ? (timeouts / sent) * 100 : 0
    return { replies, timeouts, sent, loss, min, max, avg, jit, last: rtts.length ? rtts[rtts.length - 1] : null }
  }, [lines])

  // One formatted line per result — ping/tracert-style wording, built from
  // typed fields rather than shown-as-is text. UDP's "successful" note
  // (backend-supplied: "Port unreachable (destination is reachable)") is
  // deliberately shown verbatim here rather than reworded to something
  // ICMP-flavored like "Reply from" — a UDP ping's positive result really
  // is a port-unreachable, and saying so plainly is more honest than
  // dressing it up to look like an echo reply it isn't.
  const formatLine = (l) => {
    if (l.seq <= 0) return l.note || 'Error'
    if (l.ok) return l.note
      ? `${l.note} — seq=${l.seq} time=${l.rtt_ms.toFixed(1)}ms from=${l.from}`
      : `Reply from ${l.from}: seq=${l.seq} time=${l.rtt_ms.toFixed(1)}ms`
    if (l.note === 'Destination unreachable') return `Destination unreachable${l.from ? ` (from ${l.from})` : ''} — seq=${l.seq}`
    return `Request timed out — seq=${l.seq}`
  }
  const lineClass = (l) => {
    if (l.seq <= 0) return 'pl-bad'
    return l.ok ? 'pl-ok' : 'pl-bad'
  }

  if (!toolsApi()) return <div className="toolpage"><h2>Ping</h2><ToolUnavailable /></div>
  const fmt = (v) => v == null ? '—' : v.toFixed(1)
  return (
    <div className="toolpage wide">
      <h2>Ping <span className="muted" style={{ fontWeight: 400, fontSize: 13 }}>— {isUdp ? 'UDP (native)' : isTcp ? 'TCP connect (native)' : 'ICMP echo (native)'}</span></h2>
      <div className="tool-row">
        <label style={{ flex: 1, minWidth: 260 }}>Host / IP <input className="target" value={host} placeholder="1.1.1.1, 2606:4700:4700::1111, example.com"
          onChange={(e) => setHost(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && !running && start()} /></label>
        {running ? <button onClick={stop}>■ Stop</button> : <button className="primary" onClick={start} disabled={blocked} title={blocked ? 'Resolve the setup issue below first' : undefined}>▶ Ping</button>}
      </div>
      <div className="tool-row ping-opts">
        <label>Protocol
          <select value={opts.protocol} disabled={running} onChange={(e) => {
            const protocol = e.target.value
            // Sensible default per protocol: 443 is almost always open for a
            // TCP reachability check, while UDP wants the deliberately-obscure
            // classic traceroute base port so nothing is likely listening.
            setOpts((o) => ({ ...o, protocol, destPort: protocol === 'tcp' ? 443 : 33434 }))
          }}>
            <option value="icmp">ICMP</option>
            <option value="udp">UDP</option>
            <option value="tcp">TCP</option>
          </select>
        </label>
        {usesPort && (
          <label title="A UDP ping's 'reply' is a Port-Unreachable from the destination itself — the closest UDP equivalent to a successful ICMP echo, and proof the destination is actually reachable, since nothing but the destination sends that message.">
            Port <input type="number" min="1" max="65535" value={opts.destPort} onChange={(e) => set('destPort', +e.target.value)} style={{ width: 78 }} />
          </label>
        )}
        <label>Count <input type="number" min="1" max="10000" disabled={opts.continuous} value={opts.count} onChange={(e) => set('count', +e.target.value)} style={{ width: 78 }} /></label>
        <label>Size (B) <input type="number" min="0" max="65500" value={opts.size} onChange={(e) => set('size', +e.target.value)} style={{ width: 78 }} /></label>
        <label>Timeout (ms) <input type="number" min="100" max="60000" step="100" value={opts.timeout} onChange={(e) => set('timeout', +e.target.value)} style={{ width: 90 }} /></label>
        {!usesPort && (
          <label>TTL <input type="number" min="1" max="255" value={opts.ttl} placeholder="255" onChange={(e) => set('ttl', e.target.value)} style={{ width: 66 }} /></label>
        )}
        <label>Interval (s) <input type="number" min="0.2" max="60" step="0.1" value={opts.interval} onChange={(e) => set('interval', +e.target.value)} style={{ width: 78 }} /></label>
        <label>Family
          <select value={opts.family} onChange={(e) => set('family', e.target.value)}>
            <option value="auto">Auto</option><option value="v4">IPv4</option><option value="v6">IPv6</option>
          </select>
        </label>
        <label className="cb"><input type="checkbox" checked={opts.continuous} onChange={(e) => set('continuous', e.target.checked)} />Continuous</label>
      </div>
      {usesPort && blocked && (
        <div className="proto-alert" role="alert">
          <div className="proto-alert-title">⚠ {opts.protocol.toUpperCase()} ping needs setup first</div>
          <div>This app is not running with administrator rights. Receiving the ICMP replies UDP ping depends on requires them.</div>
          <div className="proto-alert-actions">
            {caps && caps.platform === 'windows'
              ? <button onClick={restartElevated} disabled={relaunching}>{relaunching ? 'Restarting…' : 'Restart as Administrator'}</button>
              : <span className="muted" style={{ fontSize: 12 }}>Restart this app with sudo/root instead.</span>}
          </div>
        </div>
      )}
      {usesPort && !blocked && needsCapture && (
        <div className="proto-alert proto-alert-soft" role="alert">
          <div className="proto-alert-title">⚠ No packet-capture driver (Npcap) found</div>
          <div>TCP still measures this destination correctly without it — the RST/SYN-ACK reply doesn't depend on ICMP at all. Npcap is only needed for TCP path/hop discovery in the Path/MTR tab, not a direct ping like this. <a href="https://npcap.com/#download" target="_blank" rel="noreferrer" onClick={(e) => { e.preventDefault(); try { window.open('https://npcap.com/#download', '_blank') } catch {} }}>Download Npcap</a></div>
        </div>
      )}
      {usesPort && !blocked && !needsCapture && (
        <div className="proto-note" style={{ margin: '-2px 0 10px' }} title="Windows does not deliver ICMP error messages to a raw socket when the original packet came from a TCP or UDP socket. A capture driver such as Npcap sees them regardless — the same reason PingPlotter asks for it. Linux and macOS are unaffected.">
          {isTcp
            ? 'TCP ping measures the time to a SYN-ACK or RST on this port. It needs no extra software on any platform, because the answer arrives on the TCP socket itself rather than as an ICMP message. A closed port still proves reachability — the RST is a valid reply.'
            : 'UDP ping relies on the destination returning an ICMP Port-Unreachable, which requires elevated privileges to receive — run as Administrator on Windows, root on Linux/macOS.'}
        </div>
      )}
      {stats && (
        <div className="ping-stats">
          <div><span>Sent</span><b>{stats.sent}</b></div>
          <div><span>Recv</span><b>{stats.replies}</b></div>
          <div className={stats.loss > 0 ? 'danger' : ''}><span>Loss</span><b>{stats.loss.toFixed(1)}%</b></div>
          <div><span>Last</span><b>{fmt(stats.last)}</b></div>
          <div><span>Min</span><b>{fmt(stats.min)}</b></div>
          <div><span>Avg</span><b>{fmt(stats.avg)}</b></div>
          <div><span>Max</span><b>{fmt(stats.max)}</b></div>
          <div><span>Jitter</span><b>{fmt(stats.jit)}</b></div>
        </div>
      )}
      {cmd && <div className="muted" style={{ fontSize: 11, margin: '2px 0 6px', fontFamily: 'ui-monospace, monospace' }}>$ {cmd}</div>}
      <pre ref={preRef} className="tool-console">
        {lines.length
          ? lines.map((l, i) => <React.Fragment key={i}><span className={lineClass(l)}>{formatLine(l)}</span>{'\n'}</React.Fragment>)
          : 'Set options and press Ping.'}
      </pre>
    </div>
  )
}

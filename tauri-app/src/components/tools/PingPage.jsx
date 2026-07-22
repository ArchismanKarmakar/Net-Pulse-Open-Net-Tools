import React from 'react'
import { toolsApi } from '../../lib/api'
import ToolUnavailable from './ToolUnavailable'

// Native ICMP ping — the same pooled-socket/shared-dispatcher engine the
// main multi-target monitor uses (see PingRun/ping_run.hpp in the C++
// core), not the OS `ping` binary. Each result arrives from the backend
// already structured (seq/ok/rtt_ms/from/note) — this renders and computes
// stats directly from those fields; nothing here regex-matches text the way
// the old OS-process version had to.
export default function PingPage() {
  const [host, setHost] = React.useState('')
  const [opts, setOpts] = React.useState({ count: 10, size: 56, timeout: 2000, ttl: '', interval: 1, family: 'auto', continuous: false })
  const [lines, setLines] = React.useState([])
  const [running, setRunning] = React.useState(false)
  const [cmd, setCmd] = React.useState('')
  const idRef = React.useRef(null)
  const preRef = React.useRef(null)
  const set = (k, v) => setOpts((o) => ({ ...o, [k]: v }))

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
    setLines([]); setRunning(true)
    const payload = { count: opts.count, size: opts.size, timeout: opts.timeout, interval: opts.interval, family: opts.family, continuous: opts.continuous }
    if (opts.ttl !== '' && opts.ttl != null) payload.ttl = opts.ttl
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
  // typed fields rather than shown-as-is text.
  const formatLine = (l) => {
    if (l.seq <= 0) return l.note || 'Error'
    if (l.ok) return `Reply from ${l.from}: seq=${l.seq} time=${l.rtt_ms.toFixed(1)}ms`
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
      <h2>Ping <span className="muted" style={{ fontWeight: 400, fontSize: 13 }}>— ICMP echo (native)</span></h2>
      <div className="tool-row">
        <label style={{ flex: 1, minWidth: 260 }}>Host / IP <input className="target" value={host} placeholder="1.1.1.1, 2606:4700:4700::1111, example.com"
          onChange={(e) => setHost(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && !running && start()} /></label>
        {running ? <button onClick={stop}>■ Stop</button> : <button className="primary" onClick={start}>▶ Ping</button>}
      </div>
      <div className="tool-row ping-opts">
        <label>Count <input type="number" min="1" max="10000" disabled={opts.continuous} value={opts.count} onChange={(e) => set('count', +e.target.value)} style={{ width: 78 }} /></label>
        <label>Size (B) <input type="number" min="0" max="65500" value={opts.size} onChange={(e) => set('size', +e.target.value)} style={{ width: 78 }} /></label>
        <label>Timeout (ms) <input type="number" min="100" max="60000" step="100" value={opts.timeout} onChange={(e) => set('timeout', +e.target.value)} style={{ width: 90 }} /></label>
        <label>TTL <input type="number" min="1" max="255" value={opts.ttl} placeholder="255" onChange={(e) => set('ttl', e.target.value)} style={{ width: 66 }} /></label>
        <label>Interval (s) <input type="number" min="0.2" max="60" step="0.1" value={opts.interval} onChange={(e) => set('interval', +e.target.value)} style={{ width: 78 }} /></label>
        <label>Family
          <select value={opts.family} onChange={(e) => set('family', e.target.value)}>
            <option value="auto">Auto</option><option value="v4">IPv4</option><option value="v6">IPv6</option>
          </select>
        </label>
        <label className="cb"><input type="checkbox" checked={opts.continuous} onChange={(e) => set('continuous', e.target.checked)} />Continuous</label>
      </div>
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

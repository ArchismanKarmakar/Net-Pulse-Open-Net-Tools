import React from 'react'
import { toolsApi } from '../../lib/api'
import ToolUnavailable from './ToolUnavailable'

export default function DnsPage() {
  const [q, setQ] = React.useState('')
  const [fwd, setFwd] = React.useState(null)
  const [rev, setRev] = React.useState(null)
  const [busy, setBusy] = React.useState(false)
  const isIp = (s) => /:/.test(s) || /^\d{1,3}(\.\d{1,3}){3}$/.test(s)
  const run = async () => {
    const t = toolsApi(); if (!t || !q.trim()) return
    setBusy(true); setFwd(null); setRev(null)
    const query = q.trim()
    try {
      if (isIp(query)) { setRev(await t.reverse(query)); const names = (await t.reverse(query)).names; if (names && names[0]) setFwd(await t.dns(names[0])) }
      else { setFwd(await t.dns(query)); const a = (await t.dns(query)); const first = (a.a[0] || a.aaaa[0]); if (first) setRev(await t.reverse(first)) }
    } catch (e) { setFwd({ error: String(e) }) }
    setBusy(false)
  }
  if (!toolsApi()) return <div className="toolpage"><h2>DNS Lookup</h2><ToolUnavailable /></div>
  return (
    <div className="toolpage">
      <h2>DNS Lookup <span className="muted" style={{ fontWeight: 400 }}>— forward &amp; reverse</span></h2>
      <div className="tool-row">
        <label>Host or IP <input className="target" value={q} placeholder="example.com or 1.1.1.1"
          onChange={(e) => setQ(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && run()} /></label>
        <button className="primary" onClick={run} disabled={busy}>{busy ? 'Looking up…' : 'Lookup'}</button>
      </div>
      {fwd && (
        <div className="tool-card">
          <h4>Forward {fwd.name ? `(${fwd.name})` : ''}</h4>
          {fwd.error ? <div className="danger">{fwd.error}</div> : (
            <div className="dns-recs">
              <div><span className="rec-t">A</span>{(fwd.a && fwd.a.length) ? fwd.a.map((x, i) => <code key={i}>{x}</code>) : <span className="muted">none</span>}</div>
              <div><span className="rec-t">AAAA</span>{(fwd.aaaa && fwd.aaaa.length) ? fwd.aaaa.map((x, i) => <code key={i}>{x}</code>) : <span className="muted">none</span>}</div>
              {fwd.cname && fwd.cname.length > 0 && <div><span className="rec-t">CNAME</span>{fwd.cname.map((x, i) => <code key={i}>{x}</code>)}</div>}
            </div>
          )}
        </div>
      )}
      {rev && (
        <div className="tool-card">
          <h4>Reverse ({rev.addr})</h4>
          {rev.error ? <div className="muted">{rev.error}</div>
            : (rev.names && rev.names.length) ? <div className="dns-recs"><div><span className="rec-t">PTR</span>{rev.names.map((x, i) => <code key={i}>{x}</code>)}</div></div>
            : <span className="muted">no PTR record</span>}
        </div>
      )}
    </div>
  )
}

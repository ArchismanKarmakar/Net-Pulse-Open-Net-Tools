import React from 'react'
import { toolsApi } from '../../lib/api'
import ToolUnavailable from './ToolUnavailable'

export default function PortScanPage() {
  const [host, setHost] = React.useState('')
  const [start, setStart] = React.useState(1)
  const [end, setEnd] = React.useState(1024)
  const [res, setRes] = React.useState(null)
  const [busy, setBusy] = React.useState(false)
  const COMMON = { 20: 'ftp-data', 21: 'ftp', 22: 'ssh', 23: 'telnet', 25: 'smtp', 53: 'dns', 80: 'http', 110: 'pop3', 143: 'imap', 443: 'https', 445: 'smb', 3306: 'mysql', 3389: 'rdp', 5432: 'postgres', 6379: 'redis', 8080: 'http-alt', 8443: 'https-alt' }
  const run = async () => {
    const t = toolsApi(); if (!t || !host.trim()) return
    setBusy(true); setRes(null)
    setRes(await t.portscan(host.trim(), start, end))
    setBusy(false)
  }
  if (!toolsApi()) return <div className="toolpage"><h2>Port Scanner</h2><ToolUnavailable /></div>
  return (
    <div className="toolpage">
      <h2>Port Scanner <span className="muted" style={{ fontWeight: 400 }}>— TCP connect</span></h2>
      <div className="tool-row">
        <label>Host / IP <input className="target" value={host} placeholder="192.168.1.1 or example.com"
          onChange={(e) => setHost(e.target.value)} onKeyDown={(e) => e.key === 'Enter' && !busy && run()} /></label>
        <label>From <input type="number" min="1" max="65535" value={start} onChange={(e) => setStart(+e.target.value)} style={{ width: 80 }} /></label>
        <label>To <input type="number" min="1" max="65535" value={end} onChange={(e) => setEnd(+e.target.value)} style={{ width: 80 }} /></label>
        <button className="primary" onClick={run} disabled={busy}>{busy ? 'Scanning…' : 'Scan'}</button>
      </div>
      <div className="muted" style={{ fontSize: 12, margin: '2px 0 8px' }}>Ranges are capped at 2048 ports per scan. Scan only hosts you own or are authorized to test.</div>
      {res && (res.error ? <div className="danger">{res.error}</div> : (
        <div className="tool-card">
          <h4>{res.open.length} open {res.open.length === 1 ? 'port' : 'ports'} in {res.scanned[0]}–{res.scanned[1]} on {res.host}</h4>
          {res.open.length === 0 ? <span className="muted">No open TCP ports found in range.</span> : (
            <div className="ports">{res.open.map((p) => <span key={p} className="port-chip"><b>{p}</b>{COMMON[p] ? <em>{COMMON[p]}</em> : null}</span>)}</div>
          )}
        </div>
      ))}
    </div>
  )
}

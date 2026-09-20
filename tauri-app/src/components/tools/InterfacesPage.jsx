import React from 'react'
import { toolsApi, api } from '../../lib/api'
import ToolUnavailable from './ToolUnavailable'

// Full network-interface diagnostics page: every adapter on the machine —
// up or down, including loopback — with address family, operational
// status, MTU, and whether NetPulse's own engine would treat that address
// as a real, usable egress (`usable`, computed server-side by
// is_cacheable_ip() — the exact same classification the probing engine
// itself uses for the has_local_v4/has_local_v6 gate and the shared-hop
// cache, so this page can never show something as "usable" that the engine
// would actually reject as link-local/loopback noise, or vice versa).
//
// Deliberately a SEPARATE data source (`/api/interfaces/detailed`) from the
// existing `/api/interfaces` the source-address dropdown uses — that one
// stays filtered to up/non-loopback/3-field entries so its payload shape
// and behavior don't shift under this page's needs. See
// list_interfaces_detailed_json's doc comment (netpulse_ffi.hpp) for the
// full rationale.
export default function InterfacesPage() {
  const [ifaces, setIfaces] = React.useState(null) // null = loading, [] = loaded-empty
  const [error, setError] = React.useState(null)
  const [busy, setBusy] = React.useState(false)

  const refresh = React.useCallback(async () => {
    setBusy(true)
    try {
      const j = await api('/api/interfaces/detailed')
      setIfaces(Array.isArray(j) ? j : [])
      setError(null)
    } catch (e) {
      setError(String(e))
    }
    setBusy(false)
  }, [])

  React.useEffect(() => { refresh() }, [refresh])

  if (!toolsApi()) return <div className="toolpage"><h2>Network Interfaces</h2><ToolUnavailable /></div>

  const v4 = (ifaces || []).filter((i) => !i.v6)
  const v6 = (ifaces || []).filter((i) => i.v6)
  const hasUsableV4 = v4.some((i) => i.up && i.usable)
  const hasUsableV6 = v6.some((i) => i.up && i.usable)

  const Row = ({ i }) => (
    <tr className={i.up ? '' : 'muted'}>
      <td>{i.name || <span className="muted">(unnamed)</span>}</td>
      <td><code>{i.address}</code></td>
      <td>{i.v6 ? 'IPv6' : 'IPv4'}</td>
      {/* FEATURE ("add more details" to this page): adapter kind, best-effort
          (Wi-Fi/Ethernet/Virtual/Other — see NetInterface::kind's doc
          comment, transport.hpp) — this is what actually answers "which one
          of these is my Wi-Fi", instead of a generic driver-assigned name
          the person has to cross-reference against OS network settings. */}
      <td>{i.kind && i.kind !== 'Other' ? i.kind : <span className="muted">—</span>}</td>
      <td>{i.up ? <span className="ok">Up</span> : <span className="muted">Down</span>}</td>
      <td>{i.loopback ? 'Loopback' : 'Normal'}</td>
      <td>{i.mtu || <span className="muted">—</span>}</td>
      <td>{i.usable
        ? <span className="ok">Usable egress</span>
        : <span className="muted" title="Loopback, link-local, or unspecified — never a real egress, regardless of up/down status">Not usable</span>}
      </td>
    </tr>
  )

  return (
    // BUG FIX: this used to be a plain "toolpage" — capped at the same
    // 900px `max-width` as every other (prose/form) tool page. That's fine
    // for Settings/DNS/Ping, but a data TABLE doesn't want a fixed reading
    // width: on any window wider than ~950px it left a large dead strip of
    // empty background to the right of the table while the table itself,
    // still squeezed into that 900px column, needed its OWN horizontal
    // scrollbar to reach the MTU/Egress columns — scrolling sideways in a
    // narrow box with acres of unused space beside it, instead of the page
    // actually using the window it was given. "fluid" (styles.css) drops
    // the max-width so this page's content genuinely tracks the window's
    // real width, the way the rest of the app already does.
    <div className="toolpage fluid">
      <div className="toolpage-header">
        <h2>Network Interfaces</h2>
        <span className="sub">Every adapter on this machine, refreshed on demand or after a click on Refresh below.</span>
      </div>

      {!hasUsableV4 && ifaces && (
        <div className="tool-card danger">
          <b>No usable IPv4 egress detected.</b> Every IPv4 address on this
          machine is down, loopback, or link-local-only — targets set to{' '}
          <code>Family: IPv4</code> will wait rather than probe.
        </div>
      )}
      {!hasUsableV6 && ifaces && (
        <div className="tool-card danger">
          <b>No usable IPv6 egress detected.</b> Every IPv6 address on this
          machine is down, loopback, or link-local-only (this is normal on
          an IPv4-only network — every OS auto-assigns a link-local address
          to every interface whether or not real IPv6 connectivity exists) —
          targets set to <code>Family: IPv6</code> will wait rather than
          probe.
        </div>
      )}

      <div className="tool-row">
        <button onClick={refresh} disabled={busy}>{busy ? 'Refreshing…' : 'Refresh'}</button>
      </div>

      {error && <div className="tool-card danger">{error}</div>}

      {ifaces === null && !error && <div className="muted">Loading interfaces…</div>}

      {ifaces && ifaces.length === 0 && !error && (
        <div className="muted">No network interfaces found.</div>
      )}

      {ifaces && ifaces.length > 0 && (
        <div className="iface-table-wrap">
          <table className="iface-table">
            <thead>
              <tr>
                <th>Adapter</th><th>Address</th><th>Family</th><th>Kind</th><th>Status</th>
                <th>Type</th><th>MTU</th><th>Egress</th>
              </tr>
            </thead>
            <tbody>
              {ifaces.map((i, idx) => <Row key={idx} i={i} />)}
            </tbody>
          </table>
        </div>
      )}

      <div className="muted" style={{ marginTop: 14, fontSize: '0.9em' }}>
        "Usable egress" reflects exactly what NetPulse's own probing engine
        checks before starting a trace — loopback, link-local, and
        unspecified addresses are shown here for completeness but are never
        treated as real connectivity.
      </div>
    </div>
  )
}

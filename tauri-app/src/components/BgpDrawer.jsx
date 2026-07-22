import * as bgp from '../bgp'

export function RpkiBadge({ status }) {
  if (!status) return null
  const m = { valid: ['#3fb950', 'RPKI valid'], invalid: ['#f85149', 'RPKI invalid'], 'unknown': ['#d29922', 'RPKI unknown'] }
  const [c, t] = m[status] || ['#7a8699', `RPKI ${status}`]
  return <span className="chip" style={{ borderColor: c, color: c }}>{t}</span>
}

export default function BgpDrawer({ detail, onClose }) {
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
            {d.routing && d.routing.firstSeen && (
              <div className="kv"><span>First seen</span><b>{new Date(d.routing.firstSeen).toLocaleDateString()}</b></div>
            )}
            {d.routing && d.routing.observedNeighbours != null && (
              <div className="kv"><span>Peers observing</span><b>{d.routing.observedNeighbours}</b></div>
            )}
            {d.abuse && d.abuse.contacts && d.abuse.contacts.length > 0 && (
              <div className="kv"><span>Abuse contact</span><b>{d.abuse.contacts.join(', ')}</b></div>
            )}
            {d.paths && d.paths.length > 0 && (
              <div className="paths">
                <div className="paths-h">Sample BGP AS-paths (RIS)</div>
                {d.paths.map((p, i) => <div key={i} className="aspath">{p}</div>)}
              </div>
            )}
            {d.bgpState && d.bgpState.length > 0 && (
              <div className="paths">
                <div className="paths-h">Live BGP state (RIS route collectors)</div>
                {d.bgpState.map((e, i) => (
                  <div key={i} className="aspath">{e.path.join(' → ')}{e.sourceId ? ` (${e.sourceId})` : ''}</div>
                ))}
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

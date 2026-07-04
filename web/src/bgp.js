// Network-engineering enrichment via RIPEstat (free, no API key, CORS-enabled).
// All lookups are cached per-URL for the session. Everything fails soft: a
// network error or missing field yields null rather than throwing, so the UI
// degrades gracefully when offline.
//
// Docs: https://stat.ripe.net/docs/data_api

const BASE = 'https://stat.ripe.net/data'
const APP = 'sourceapp=netpulse'
const cache = new Map()

// RFC1918 / CGNAT / loopback / link-local / ULA etc. — never sent to RIPEstat.
export function isPublicIp(ip) {
  if (!ip || ip === '*' || ip === '' || ip === '—') return false
  if (ip.includes(':')) {
    const l = ip.toLowerCase()
    if (l === '::1' || l === '::') return false
    if (l.startsWith('fe8') || l.startsWith('fe9') || l.startsWith('fea') || l.startsWith('feb')) return false // fe80::/10
    if (l.startsWith('fc') || l.startsWith('fd')) return false // fc00::/7 ULA
    return true
  }
  const p = ip.split('.').map(Number)
  if (p.length !== 4 || p.some((n) => Number.isNaN(n))) return false
  const [a, b] = p
  if (a === 10 || a === 127 || a === 0) return false
  if (a === 169 && b === 254) return false
  if (a === 172 && b >= 16 && b <= 31) return false
  if (a === 192 && b === 168) return false
  if (a === 100 && b >= 64 && b <= 127) return false // CGNAT 100.64/10
  if (a >= 224) return false // multicast / reserved
  return true
}

function getJson(endpoint, resource, extra = '') {
  const url = `${BASE}/${endpoint}/data.json?resource=${encodeURIComponent(resource)}&${APP}${extra}`
  if (cache.has(url)) return cache.get(url)
  const p = fetch(url)
    .then((r) => (r.ok ? r.json() : null))
    .then((j) => (j && j.data) || null)
    .catch(() => null)
  cache.set(url, p)
  return p
}

// Lightweight per-hop annotation: { asn, holder, prefix } (any field may be null)
export async function ipInfo(ip) {
  const ni = await getJson('network-info', ip)
  const asn = ni && ni.asns && ni.asns.length ? ni.asns[0] : null
  const prefix = (ni && ni.prefix) || null
  if (!asn) return { asn: null, holder: null, prefix }
  const ov = await getJson('as-overview', `AS${asn}`)
  return { asn, holder: (ov && ov.holder) || null, prefix }
}

// Full drawer details: ASN, holder, prefix, RPKI status, geo, sample BGP paths.
export async function hopDetails(ip) {
  const ni = await getJson('network-info', ip)
  const asn = ni && ni.asns && ni.asns.length ? ni.asns[0] : null
  const prefix = (ni && ni.prefix) || null
  const [ov, rpki, geo, lg, whois] = await Promise.all([
    asn ? getJson('as-overview', `AS${asn}`) : null,
    asn && prefix ? getJson('rpki-validation', `AS${asn}`, `&prefix=${encodeURIComponent(prefix)}`) : null,
    getJson('maxmind-geo-lite', ip),
    prefix ? getJson('looking-glass', prefix) : null,
    getJson('whois', ip),
  ])

  // distinct sample AS-paths from RIS route collectors
  let paths = []
  if (lg && lg.rrcs) {
    for (const rrc of lg.rrcs) {
      for (const peer of rrc.peers || []) {
        if (peer.as_path) paths.push(peer.as_path.trim())
      }
    }
  }
  paths = [...new Set(paths)].slice(0, 6)

  let country = null
  let city = null
  if (geo && geo.located_resources && geo.located_resources[0]) {
    const loc = geo.located_resources[0].locations && geo.located_resources[0].locations[0]
    if (loc) {
      country = loc.country || null
      city = loc.city || null
    }
  }

  // pull a couple of human-readable whois fields if present
  let netname = null
  let descr = null
  if (whois && whois.records) {
    for (const rec of whois.records) {
      for (const attr of rec) {
        if (!netname && /netname/i.test(attr.key)) netname = attr.value
        if (!descr && /descr|org-?name|owner/i.test(attr.key)) descr = attr.value
      }
    }
  }

  return {
    ip,
    asn,
    prefix,
    holder: (ov && ov.holder) || null,
    announced: ov ? ov.announced : null,
    rpki: (rpki && rpki.status) || null,
    country,
    city,
    netname,
    descr,
    paths,
  }
}

// Convenience external links network engineers reach for.
export const links = {
  heAsn: (asn) => `https://bgp.he.net/AS${asn}`,
  heIp: (ip) => `https://bgp.he.net/ip/${ip}`,
  peeringdb: (asn) => `https://www.peeringdb.com/asn/${asn}`,
  ripestat: (r) => `https://stat.ripe.net/${r}`,
}

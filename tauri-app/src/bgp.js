// Network-engineering enrichment via RIPEstat (free, no API key, CORS-enabled).
// All lookups are cached per-URL for the session. Everything fails soft: a
// network error or missing field yields null rather than throwing, so the UI
// degrades gracefully when offline.
//
// Docs: https://stat.ripe.net/docs/data_api

const BASE = 'https://stat.ripe.net/data'
const APP = 'sourceapp=netpulse'
// Every distinct (endpoint, resource) pair this module has ever looked up —
// one entry per unique URL, across every hop IP / ASN / prefix the app has
// ever shown a badge or drawer for, for as long as the page stays open. A
// long-running session that visits many targets with real-world route churn
// (ECMP, CDN edge rotation, IPv6 privacy addresses) can rack up hundreds to
// thousands of distinct hop IPs over hours, and some responses here (whois
// records, looking-glass AS-path tables) aren't small. Uncapped, this was a
// genuine unbounded-growth leak, not a bug in any one lookup. Capped with a
// simple FIFO eviction (Map preserves insertion order) — not true LRU, but
// cheap, and anything actually still relevant gets re-fetched/re-cached
// transparently on its next lookup since getJson() only ever checks
// cache.has() first.
const CACHE_MAX = 800
const cache = new Map()
function cacheSet(url, value) {
  cache.set(url, value)
  if (cache.size > CACHE_MAX) {
    const excess = cache.size - CACHE_MAX
    const it = cache.keys()
    for (let i = 0; i < excess; i++) cache.delete(it.next().value)
  }
}

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
  cacheSet(url, p)
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

// Live AS-path / next-hop table for a prefix, straight from RIS route
// collectors (distinct from the sampled paths in looking-glass below).
export async function bgpState(prefix) {
  if (!prefix) return null
  const d = await getJson('bgp-state', prefix)
  if (!d || !Array.isArray(d.bgp_state)) return null
  return d.bgp_state.slice(0, 8).map((e) => ({
    path: Array.isArray(e.path) ? e.path : [],
    community: Array.isArray(e.community) ? e.community : [],
    sourceId: e.source_id || null,
  }))
}

// Same call as bgpState(), but ALWAYS issues a live fetch instead of
// short-circuiting through the permanent per-URL `cache` above. Route-leak
// detection needs to observe change over time (has the origin/upstream ASN
// shifted since we last looked?) — the cached bgpState() would forever
// return the first answer it ever saw for a prefix, which would make every
// leak check compare the baseline against itself and never fire.
export async function bgpStateFresh(prefix) {
  if (!prefix) return null
  const url = `${BASE}/bgp-state/data.json?resource=${encodeURIComponent(prefix)}&${APP}`
  const d = await fetch(url)
    .then((r) => (r.ok ? r.json() : null))
    .then((j) => (j && j.data) || null)
    .catch(() => null)
  if (!d || !Array.isArray(d.bgp_state)) return null
  return d.bgp_state.slice(0, 8).map((e) => ({
    path: Array.isArray(e.path) ? e.path : [],
    community: Array.isArray(e.community) ? e.community : [],
    sourceId: e.source_id || null,
  }))
}

// Reduces a bgpStateFresh()-shaped array down to { originAsn, upstreamAsns }
// for route-leak comparison: origin = last hop of each path (closest to the
// prefix), upstream = second-to-last hop (who's announcing it to the world).
// Takes the MAJORITY origin across all reporting peers/paths rather than just
// the first path, since a single stale/minority peer shouldn't flip the
// baseline or trigger a false alert.
export function summarizeAsPaths(paths) {
  if (!paths || !paths.length) return null
  const originCounts = new Map()
  const upstreamsByOrigin = new Map()
  for (const p of paths) {
    if (!p.path || p.path.length < 1) continue
    const origin = p.path[p.path.length - 1]
    const upstream = p.path.length >= 2 ? p.path[p.path.length - 2] : null
    originCounts.set(origin, (originCounts.get(origin) || 0) + 1)
    if (upstream) {
      if (!upstreamsByOrigin.has(origin)) upstreamsByOrigin.set(origin, new Set())
      upstreamsByOrigin.get(origin).add(upstream)
    }
  }
  if (!originCounts.size) return null
  let originAsn = null
  let best = -1
  for (const [asn, count] of originCounts) {
    if (count > best) { best = count; originAsn = asn }
  }
  return { originAsn, upstreamAsns: upstreamsByOrigin.get(originAsn) || new Set() }
}

// Compares a fresh AS-path summary against a previously captured baseline
// and returns a short human-readable alert string, or null if nothing looks
// wrong. Two distinct signals, per RFC-leak-alerting best practice:
//  - origin ASN changed entirely -> possible hijack (someone else now
//    originates a prefix that used to be ours).
//  - same origin, but a NEW upstream not seen in the baseline -> possible
//    leak (the prefix is now reaching the world through an unexpected path).
export function detectRouteLeak(baseline, fresh) {
  if (!baseline || !fresh || !fresh.originAsn) return null
  if (fresh.originAsn !== baseline.originAsn) {
    return `origin ASN changed: AS${baseline.originAsn} → AS${fresh.originAsn}`
  }
  for (const up of fresh.upstreamAsns) {
    if (!baseline.upstreamAsns.has(up)) {
      return `unexpected upstream ASN AS${up}`
    }
  }
  return null
}

// Visibility / first-seen for a prefix — answers "is this actually announced,
// and since when" independent of the per-ASN as-overview.
export async function routingStatus(prefix) {
  if (!prefix) return null
  const d = await getJson('routing-status', prefix)
  if (!d) return null
  return {
    announced: !!d.announced,
    firstSeen: (d.first_seen && d.first_seen.time) || null,
    observedNeighbours: d.observed_neighbours ?? null,
    visibility: d.visibility || null,
  }
}

// Other prefixes announced by an ASN — used for "this network also announces
// N other prefixes" context rather than a full prefix list in the drawer.
export async function announcedPrefixes(asn) {
  if (!asn) return null
  const d = await getJson('announced-prefixes', `AS${asn}`)
  if (!d || !Array.isArray(d.prefixes)) return null
  return d.prefixes.map((p) => p.prefix).filter(Boolean)
}

// ASNs registered/routed in a country — a coarser, network/country-level tool
// (not tied to a specific hop), exposed for the Tools area.
export async function countryAsns(countryCode) {
  if (!countryCode) return null
  const d = await getJson('country-asns', countryCode, '&lod=1')
  if (!d || !Array.isArray(d.countries) || !d.countries[0]) return null
  const c = d.countries[0]
  const routed = (c.asns && c.asns.routed) || []
  return { country: c.country || countryCode, routedCount: routed.length, routed: routed.slice(0, 50) }
}

// Abuse-contact lookup for an IP/prefix — the "who do I report this to" tool.
export async function abuseContact(resource) {
  if (!resource) return null
  const d = await getJson('abuse-contact-finder', resource)
  if (!d) return null
  return { contacts: Array.isArray(d.abuse_contacts) ? d.abuse_contacts : [], rir: d.authoritative_rir || null }
}

// Full drawer details: ASN, holder, prefix, RPKI status, geo, sample BGP paths.
export async function hopDetails(ip) {
  const ni = await getJson('network-info', ip)
  const asn = ni && ni.asns && ni.asns.length ? ni.asns[0] : null
  const prefix = (ni && ni.prefix) || null
  const [ov, rpki, geo, lg, whois, routing, abuse, bgpst] = await Promise.all([
    asn ? getJson('as-overview', `AS${asn}`) : null,
    asn && prefix ? getJson('rpki-validation', `AS${asn}`, `&prefix=${encodeURIComponent(prefix)}`) : null,
    getJson('maxmind-geo-lite', ip),
    prefix ? getJson('looking-glass', prefix) : null,
    getJson('whois', ip),
    prefix ? routingStatus(prefix) : null,
    abuseContact(ip),
    prefix ? bgpState(prefix) : null,
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
    routing,
    abuse,
    bgpState: bgpst,
  }
}

// Convenience external links network engineers reach for.
export const links = {
  heAsn: (asn) => `https://bgp.he.net/AS${asn}`,
  heIp: (ip) => `https://bgp.he.net/ip/${ip}`,
  peeringdb: (asn) => `https://www.peeringdb.com/asn/${asn}`,
  ripestat: (r) => `https://stat.ripe.net/${r}`,
}
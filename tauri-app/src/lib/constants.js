// Shared constants used across the dashboard, detail view, and edit panel.

export const FOCUS = [
  ['Last 5 sec', 5], ['Last 10 sec', 10], ['Last 30 sec', 30],
  ['Last 1 min', 60], ['Last 2 min', 120], ['Last 5 min', 300], ['Last 10 min', 600],
  ['Last 30 min', 1800], ['Last 1 hour', 3600], ['Last 3 hours', 10800],
  ['Last 6 hours', 21600], ['Last 12 hours', 43200], ['Last 24 hours', 86400], ['All', 'all'],
]

export const SCALE_STEPS = [25, 50, 75, 100, 150, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 5000]

// ── Config field limits ─────────────────────────────────────────────────────
// Mirrors the engine's clamps (napi.cpp). The engine still clamps defensively,
// but validating here gives immediate feedback and stops obviously-bad values.
export const CFG_LIMITS = {
  probe:   { min: 0.1, max: 3600,  step: 0.1, label: 'Probe interval (s)' },
  trace:   { min: 1,   max: 86400, step: 1,   label: 'Trace interval (s)' },
  timeout: { min: 0,   max: 3600,  step: 0.1, label: 'Timeout (s, 0 = auto)' },
  payload: { min: 0,   max: 65500, step: 1,   label: 'Payload (bytes)' },
  maxhops: { min: 1,   max: 255,   step: 1,   label: 'Max hops' },
  // Remote port — editable for TCP/UDP only; validateCfg (below) skips it
  // for protocols where it doesn't apply (icmp) via the same "only check
  // fields actually present" rule every other field already follows.
  destPort: { min: 1,  max: 65535, step: 1,   label: 'Remote port' },
}

// Returns a { field: message } map of any out-of-range / non-numeric values.
export function validateCfg(cfg) {
  const errs = {}
  for (const k of Object.keys(CFG_LIMITS)) {
    if (cfg[k] == null || cfg[k] === '') continue
    const v = Number(cfg[k]); const L = CFG_LIMITS[k]
    if (!Number.isFinite(v)) errs[k] = `${L.label} must be a number`
    else if (v < L.min || v > L.max) errs[k] = `${L.label} must be between ${L.min} and ${L.max}`
  }
  return errs
}

// Ping tab's own field limits — separate from CFG_LIMITS above (MTR/Edit
// Config) because the fields genuinely differ: count/size/timeout(ms)/
// interval(s) here have no equivalent there, and MTR's timeout is in
// SECONDS while Ping's is in MILLISECONDS — reusing one shared object
// would risk silently validating one against the other's units. destPort
// is intentionally NOT duplicated here — PingPage imports CFG_LIMITS.destPort
// directly for that one, since a remote port is the same concept and the
// same valid range in both places.
export const PING_LIMITS = {
  count:    { min: 1,   max: 10000, label: 'Count' },
  size:     { min: 0,   max: 65500, label: 'Size (B)' },
  timeout:  { min: 100, max: 60000, label: 'Timeout (ms)' },
  interval: { min: 0.2, max: 60,    label: 'Interval (s)' },
  ttl:      { min: 1,   max: 255,   label: 'TTL' },
}

// Same shape and behavior as validateCfg() above, scoped to PING_LIMITS.
export function validatePingCfg(cfg) {
  const errs = {}
  for (const k of Object.keys(PING_LIMITS)) {
    if (cfg[k] == null || cfg[k] === '') continue
    const v = Number(cfg[k]); const L = PING_LIMITS[k]
    if (!Number.isFinite(v)) errs[k] = `${L.label} must be a number`
    else if (v < L.min || v > L.max) errs[k] = `${L.label} must be between ${L.min} and ${L.max}`
  }
  if (cfg.destPort != null && cfg.destPort !== '') {
    const v = Number(cfg.destPort); const L = CFG_LIMITS.destPort
    if (!Number.isFinite(v)) errs.destPort = `${L.label} must be a number`
    else if (v < L.min || v > L.max) errs.destPort = `${L.label} must be between ${L.min} and ${L.max}`
  }
  return errs
}

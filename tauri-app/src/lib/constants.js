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

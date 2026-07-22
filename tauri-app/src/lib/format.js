import { SCALE_STEPS } from './constants'

export const fmt = (v) => (v === null || v === undefined ? '—' : Number(v).toFixed(1))
export const timeFmt = (s) => new Date(s * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
// Relative "how long ago" for a unix-seconds timestamp — used for the stale-
// hop display ("last seen 4m ago"). Coarsens as the gap grows (seconds only
// under a minute, minutes under an hour, hours beyond) since a stale hop
// that's been dark for days doesn't need second-level precision.
export function agoFmt(s) {
  if (s == null) return ''
  const secs = Math.max(0, Date.now() / 1000 - s)
  if (secs < 5) return 'just now'
  if (secs < 60) return `${Math.round(secs)}s ago`
  if (secs < 3600) return `${Math.round(secs / 60)}m ago`
  if (secs < 86400) return `${Math.round(secs / 3600)}h ago`
  return `${Math.round(secs / 86400)}d ago`
}

export function hopColor(i, sel) {
  if (sel) return '#3a82f6'
  const h = (i * 0.61803398875) % 1
  return `hsl(${Math.round(h * 360)}, 60%, 60%)`
}

export function latColor(ms, alertMs) {
  if (ms == null) return '#7a8699'
  const t = Math.max(0, Math.min(1, ms / Math.max(1, alertMs)))
  const r = t < 0.5 ? Math.round(t * 2 * 255) : 230
  const g = t < 0.5 ? 200 : Math.round(200 * (1 - (t - 0.5) * 2))
  return `rgb(${r},${g},60)`
}

export function lossBg(loss) {
  return loss > 0 ? `rgba(248,81,73,${(0.10 + 0.24 * loss / 100).toFixed(3)})` : 'rgba(63,185,80,0.10)'
}

export function niceCeil(v) {
  for (const s of SCALE_STEPS) if (v <= s) return s
  return Math.ceil(v / 1000) * 1000
}

export function download(name, text, type = 'text/plain') {
  const blob = new Blob([text], { type })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a'); a.href = url; a.download = name; a.click()
  URL.revokeObjectURL(url)
}
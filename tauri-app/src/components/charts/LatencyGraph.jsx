import { latColor, lossBg } from '../../lib/format'

// PingPlotter-style shared-scale latency graph: min–max whisker, avg circle,
// cur ✕, row shaded by loss, and a red line threading consecutive hops.
export default function LatencyGraph({ h, prevAvg, nextAvg, scaleMax, alertMs, w = 360, hgt = 24 }) {
  const X = (ms) => Math.max(0, Math.min(1, ms / scaleMax)) * (w - 10) + 5
  const yc = hgt / 2
  // The connector to the previous/next row crosses the row boundary exactly
  // halfway (yc is equidistant from 0/hgt and from the neighbour row's own
  // yc), so the boundary x must be the MIDPOINT between this row's avg and
  // the neighbour's avg — not the neighbour's raw x position. Using the
  // neighbour's raw x here made row i's bottom segment end at a different x
  // than row i+1's top segment began at, so the "thread" jumped sideways at
  // every row seam instead of connecting into one continuous diagonal.
  const xPrevMid = h.avg != null && prevAvg != null ? (X(prevAvg) + X(h.avg)) / 2 : null
  const xNextMid = h.avg != null && nextAvg != null ? (X(h.avg) + X(nextAvg)) / 2 : null
  return (
    <svg width={w} height={hgt} className="latgraph">
      <rect x="0" y="0" width={w} height={hgt} fill={lossBg(h.loss)} />
      {xPrevMid != null && <line x1={xPrevMid} y1="0" x2={X(h.avg)} y2={yc} stroke="#e5484d" strokeWidth="1.4" />}
      {xNextMid != null && <line x1={X(h.avg)} y1={yc} x2={xNextMid} y2={hgt} stroke="#e5484d" strokeWidth="1.4" />}
      {h.min != null && h.max != null && (
        <g stroke="#9aa7b8">
          <line x1={X(h.min)} x2={X(h.max)} y1={yc} y2={yc} />
          <line x1={X(h.min)} x2={X(h.min)} y1={yc - 4} y2={yc + 4} />
          <line x1={X(h.max)} x2={X(h.max)} y1={yc - 4} y2={yc + 4} />
        </g>
      )}
      {h.avg != null && <circle cx={X(h.avg)} cy={yc} r="3.6" fill="#0e1116" stroke={latColor(h.avg, alertMs)} strokeWidth="1.8" />}
      {h.cur != null && <text x={X(h.cur)} y={yc + 4} fill={latColor(h.cur, alertMs)} fontSize="12" textAnchor="middle">✕</text>}
    </svg>
  )
}

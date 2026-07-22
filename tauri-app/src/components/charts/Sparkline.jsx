import React from 'react'

const Sparkline = React.memo(function Sparkline({ points, color, w = 96, h = 22 }) {
  if (!points || points.length < 2) return <svg width={w} height={h} />
  const rtts = points.filter((p) => p[1] != null).map((p) => p[1])
  if (rtts.length < 2) return <svg width={w} height={h} />
  const lo = Math.min(...rtts), hi = Math.max(...rtts), span = hi - lo || 1, n = points.length
  const xy = (i, v) => [((i / (n - 1)) * (w - 2) + 1).toFixed(1), (h - 1 - ((v - lo) / span) * (h - 2)).toFixed(1)]
  const segs = []; let cur = []
  points.forEach((p, i) => { if (p[1] == null) { if (cur.length) { segs.push(cur); cur = [] } } else cur.push(xy(i, p[1]).join(',')) })
  if (cur.length) segs.push(cur)
  return (
    <svg width={w} height={h}>
      {points.map((p, i) => p[1] == null
        ? <line key={i} x1={(i / (n - 1)) * (w - 2) + 1} x2={(i / (n - 1)) * (w - 2) + 1} y1="0" y2={h} stroke="rgba(240,70,70,.3)" />
        : null)}
      {segs.map((s, i) => <polyline key={i} points={s.join(' ')} fill="none" stroke={color} strokeWidth="1.3" />)}
    </svg>
  )
})

export default Sparkline

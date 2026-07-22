// A ring (not a filled disc) reads as a gauge/indicator rather than a plain
// bullet, and the partial arc (via strokeDasharray) gives a little texture
// even at a glance — closed circle for a clean ok/steady state, a gapped
// ring for discovering/settling to hint "still resolving".
const RING_COLOR = {
  ok: 'var(--good)', okloss: 'var(--good-soft)', settling: 'var(--lime, var(--good-soft))',
  warn: 'var(--warn-soft)', bad: 'var(--bad)', down: 'var(--danger)',
  discovering: 'var(--accent)',
}

export default function StatusRing({ status, size = 40 }) {
  const color = RING_COLOR[status] || 'var(--faint)'
  const r = (size - 6) / 2
  const c = 2 * Math.PI * r
  const gapped = status === 'discovering' || status === 'settling'
  return (
    <svg width={size} height={size} className={'status-ring st-' + status} viewBox={`0 0 ${size} ${size}`} aria-label={`status: ${status}`}>
      <circle cx={size / 2} cy={size / 2} r={r} fill="none" stroke="var(--border2)" strokeWidth="3" />
      <circle
        cx={size / 2} cy={size / 2} r={r} fill="none" stroke={color} strokeWidth="3" strokeLinecap="round"
        strokeDasharray={gapped ? `${c * 0.72} ${c * 0.28}` : `${c} 0`}
        transform={`rotate(-90 ${size / 2} ${size / 2})`}
      />
    </svg>
  )
}

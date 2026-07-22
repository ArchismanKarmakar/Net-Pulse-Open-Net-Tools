// Self-contained per-hop colour classification (silent/loss/ok/down) — kept
// independent of the app's alerts.ms/alerts.loss thresholds on purpose: this
// is a compressed at-a-glance topology strip, not the detailed hop table, so
// it only needs to distinguish "not replying" / "some loss" / "clean" /
// "the destination itself is unreachable".
function hopDotStatus(h) {
  const silent = h.sent > 0 && h.recv === 0
  if (h.is_dest && silent) return 'down'
  if (silent) return 'silent'
  if (h.loss > 0) return 'loss'
  return 'ok'
}

export default function MiniVisualPath({ hops }) {
  if (!hops || !hops.length) {
    return <div className="mvp mvp-empty muted">resolving route…</div>
  }
  const dest = hops.find((h) => h.is_dest) || hops[hops.length - 1]
  const first = hops[0]
  const middleCount = Math.max(0, hops.length - (dest === first ? 1 : 2))

  return (
    <div className="mvp" title={`${hops.length} hop${hops.length === 1 ? '' : 's'} to destination`}>
      <span className="mvp-node mvp-host" title="Your Host">🖥</span>
      <span className="mvp-line" />
      <span className={'mvp-node mvp-hop mvp-' + hopDotStatus(first)} title={`Hop ${first.hop} — ${first.address || '*'}`}>{first.hop}</span>
      {middleCount > 0 && (
        <>
          <span className="mvp-line mvp-line-dashed" />
          <span className="mvp-more" title={`${middleCount} intermediate hop${middleCount === 1 ? '' : 's'}`}>{middleCount}</span>
        </>
      )}
      {dest !== first && (
        <>
          <span className="mvp-line" />
          <span className={'mvp-node mvp-hop mvp-dest mvp-' + hopDotStatus(dest)} title={`Hop ${dest.hop} — destination (${dest.address || '*'})`}>{dest.hop}</span>
        </>
      )}
      <span className="mvp-line" />
      <span className="mvp-node mvp-target" title="Destination">🎯</span>
    </div>
  )
}

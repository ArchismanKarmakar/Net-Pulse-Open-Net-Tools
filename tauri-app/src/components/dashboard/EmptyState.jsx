// Small inline network illustration — built to match the app's own dark
// instrument-panel palette (CSS variables) rather than a bitmap asset, so it
// themes correctly in light mode too and never blurs on HiDPI screens.
function NetworkIllustration() {
  const nodes = [
    [140, 30], [40, 70], [110, 100], [180, 90], [240, 60], [220, 120], [160, 150],
  ]
  const edges = [[0, 1], [0, 2], [0, 3], [3, 4], [3, 5], [2, 6], [5, 6]]
  return (
    <svg width="260" height="170" viewBox="0 0 260 170" aria-hidden="true">
      {edges.map(([a, b], i) => (
        <line key={i} x1={nodes[a][0]} y1={nodes[a][1]} x2={nodes[b][0]} y2={nodes[b][1]}
          stroke="var(--border2)" strokeWidth="1.5" strokeDasharray="3 4" />
      ))}
      {nodes.map(([x, y], i) => (
        <g key={i}>
          <circle cx={x} cy={y} r={i === 0 ? 15 : 11} fill="var(--panel2)" stroke="var(--border2)" strokeWidth="1.5" />
          <text x={x} y={y + 4} textAnchor="middle" fontSize={i === 0 ? 13 : 11} fill="var(--faint)">?</text>
        </g>
      ))}
    </svg>
  )
}

// Quick-start shortcuts — each one performs a real action (adds a live trace
// or switches tools) rather than being a static label, so the empty state
// doubles as an onboarding flow.
export default function EmptyState({ onQuickAdd, onOpenTool }) {
  return (
    <div className="dash-empty-state">
      <NetworkIllustration />
      <div className="dash-empty-title">No targets added yet</div>
      <div className="muted">Start network diagnostics and monitoring by adding your first target above.</div>
      <div className="dash-guide-row">
        <button onClick={() => onQuickAdd('1.1.1.1')}>Guide: Trace Cloudflare (1.1.1.1)</button>
        <button onClick={() => onQuickAdd('8.8.8.8')}>Guide: Ping Google DNS (8.8.8.8)</button>
        <button onClick={() => onOpenTool('dns')}>Guide: Check DNS propagation</button>
        <button onClick={() => onOpenTool('ports')}>Open Port Scanner</button>
      </div>
    </div>
  )
}

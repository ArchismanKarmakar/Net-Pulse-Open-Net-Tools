const COLS = [
  ['custom', '⠿', 'Order'], ['status', '●', 'Status'], ['name', '🔤', 'Name'],
  ['latency', '⏱', 'Latency'], ['loss', '📉', 'Loss %'], ['jitter', '〰', 'Jitter'], ['hops', '⛓', 'Hops'],
]

export default function SortBar({ dashSortCol, dashSortDir, toggleDashSort }) {
  return (
    <div className="sort-bar">
      <span className="sort-bar-label">Sort by</span>
      {COLS.map(([col, icon, label]) => (
        <button
          key={col}
          className={'sort-pill' + (dashSortCol === col ? ' active' : '')}
          onClick={() => toggleDashSort(col)}
        >
          <span className="sort-pill-icon" aria-hidden="true">{icon}</span>
          {label}
          {dashSortCol === col && <span className="sort-arrow">{dashSortDir > 0 ? ' ▲' : ' ▼'}</span>}
        </button>
      ))}
    </div>
  )
}

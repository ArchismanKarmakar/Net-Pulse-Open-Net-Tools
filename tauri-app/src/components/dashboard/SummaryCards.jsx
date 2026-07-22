function Card({ icon, label, value, tone, active, onClick }) {
  return (
    <button
      className={'dcard' + (tone ? ` dcard-${tone}` : '') + (active ? ' dcard-active' : '')}
      onClick={onClick}
      title={onClick ? 'Click to filter the list by this status' : undefined}
    >
      <span className="dcard-icon">{icon}</span>
      <div className="dcard-body">
        <div className="dcard-label">{label}</div>
        <div className="dcard-value">{value}</div>
      </div>
    </button>
  )
}

function LampGroup({ dim, label, summary, statusFilter, toggleStatusFilter }) {
  const isActive = (status) => statusFilter && statusFilter.dim === dim && statusFilter.status === status
  return (
    <div className="dcard-group">
      <div className="dcard-group-label">{label}</div>
      <div className="dcard-group-cards">
        <Card icon="✓" label="Healthy" value={summary.ok} tone="ok" active={isActive('ok')} onClick={() => toggleStatusFilter(dim, 'ok')} />
        {dim === 'dest' && (
          <Card icon="✦" label="Settling" value={summary.settling} tone="settling" active={isActive('settling')} onClick={() => toggleStatusFilter(dim, 'settling')} />
        )}
        <Card icon="▲" label="Warning" value={summary.warn} tone="warn" active={isActive('warn')} onClick={() => toggleStatusFilter(dim, 'warn')} />
        <Card icon="◆" label="Degraded" value={summary.bad} tone="bad" active={isActive('bad')} onClick={() => toggleStatusFilter(dim, 'bad')} />
        <Card icon="●" label="Down" value={summary.down} tone="down" active={isActive('down')} onClick={() => toggleStatusFilter(dim, 'down')} />
        {summary.discovering > 0 && (
          <Card icon="◐" label="Discovering" value={summary.discovering} tone="discovering" active={isActive('discovering')} onClick={() => toggleStatusFilter(dim, 'discovering')} />
        )}
      </div>
    </div>
  )
}

export default function SummaryCards({ destSummary, pathSummary, statusFilter, toggleStatusFilter }) {
  return (
    <div className="dashboard-cards">
      <Card icon="📡" label="Targets" value={destSummary.total} />
      <LampGroup dim="dest" label="Destination" summary={destSummary} statusFilter={statusFilter} toggleStatusFilter={toggleStatusFilter} />
      <LampGroup dim="path" label="Path" summary={pathSummary} statusFilter={statusFilter} toggleStatusFilter={toggleStatusFilter} />
    </div>
  )
}

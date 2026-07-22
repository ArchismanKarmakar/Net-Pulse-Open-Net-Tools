import SummaryCards from './SummaryCards'
import SortBar from './SortBar'
import EmptyState from './EmptyState'
import DashboardTable from './DashboardTable'

export default function Dashboard({
  targets, dashboardSummary, dashboardRows, targetSearch, setTargetSearch,
  onQuickAdd, onOpenTool, dashSortCol, dashSortDir, toggleDashSort, ...tableProps
}) {
  return (
    <div className="dashboard">
      <div className="dashboard-head">
        <SummaryCards summary={dashboardSummary} />

        <div className="dashboard-toolbar">
          <div className="dash-search-row">
            <div className="dash-search-wrap">
              <span className="dash-search-icon" aria-hidden="true">🔍</span>
              <input
                type="search"
                className="dash-search-lg"
                placeholder="Filter by name or IP…"
                value={targetSearch}
                onChange={(e) => setTargetSearch(e.target.value)}
                disabled={targets.length === 0}
              />
            </div>
            <span className="muted dash-count">
              {targets.length === 0 ? 'No targets yet' : `${dashboardRows.length} of ${targets.length} target${targets.length === 1 ? '' : 's'}`}
            </span>
          </div>
          {targets.length > 0 && (
            <SortBar dashSortCol={dashSortCol} dashSortDir={dashSortDir} toggleDashSort={toggleDashSort} />
          )}
        </div>
      </div>

      {targets.length === 0 ? (
        <EmptyState onQuickAdd={onQuickAdd} onOpenTool={onOpenTool} />
      ) : targetSearch.trim() && dashboardRows.length === 0 ? (
        <div className="empty">No targets match "{targetSearch}"</div>
      ) : (
        <DashboardTable rows={dashboardRows} {...tableProps} />
      )}
    </div>
  )
}

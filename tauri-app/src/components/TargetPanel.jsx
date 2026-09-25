import { Reorder, useDragControls } from 'motion/react'
import { IconSave, IconUpload, IconDataObject, IconTableChart } from './icons/MaterialIcons'
import SummaryCards from './dashboard/SummaryCards'
import SortBar from './dashboard/SortBar'
import EmptyState from './dashboard/EmptyState'
import TargetCard from './TargetCard'

// useDragControls() is a hook, so it can only be called once per rendered
// item — this thin wrapper is what makes that possible per-row inside a
// .map(). dragControls is still handed down into TargetCard so the ⠿ icon
// keeps working as an explicit grab point, but dragListener is left at its
// default (true) so Reorder.Item itself also listens for a pointer-down
// anywhere on the card — dragging no longer requires hitting that small
// icon precisely. Motion's drag gesture has its own built-in movement
// threshold before a drag actually starts, so a plain tap/click elsewhere
// on the card (selecting the target, pressing a button) still registers
// normally instead of being swallowed as a drag.
function ReorderableTargetCard({ t, ...cardProps }) {
  const dragControls = useDragControls()
  return (
    <Reorder.Item as="div" value={t} dragControls={dragControls} className="target-card-reorder-item">
      <TargetCard t={t} dragControls={dragControls} {...cardProps} />
    </Reorder.Item>
  )
}

export default function TargetPanel({
  compact, sidebarWidth, isResizing, startDrag, panelRef,
  targets, rows, destSummary, pathSummary, statusFilter, toggleStatusFilter,
  targetSearch, setTargetSearch,
  dashSortCol, dashSortDir, toggleDashSort, selectDashSort,
  onQuickAdd, onOpenTool,
  onReorder, moveTarget,
  forceRecheckOne, pauseOne, removeTarget, openTarget,
  targetState, destLamp, pathLamp, destHopOf, familyLabel, fmt, STATE_LABEL,
  view, alertMs, sel,
  allPaused, pauseAll, exportTargetList, exportTargetsJson, onLoadListClick, exportAllTargetsFullXlsx, xlsxExportProgress,
}) {
  return (
    <div
      ref={panelRef}
      className={'target-panel' + (compact ? ' compact' : '') + (isResizing ? ' resizing' : '')}
      style={compact ? { width: sidebarWidth } : undefined}
    >
      {compact ? (
        <>
          <div className="strip-head">
            <button className="strip-back" title="Back to full dashboard" onClick={() => openTarget(null)}>← Dashboard</button>
            <h3>Targets</h3>
            {targets.length > 0 && (
              <span className="strip-counts" title="Destination: Healthy / Warning / Down">
                <span className="sc-ok">{destSummary.ok}</span>
                <span className="sc-warn">{destSummary.warn}</span>
                <span className="sc-down">{destSummary.down}</span>
              </span>
            )}
          </div>
          <div className="target-panel-toolbar">
            <div className="dash-search-wrap">
              <span className="dash-search-icon" aria-hidden="true">🔍</span>
              <input
                type="search"
                className="dash-search"
                placeholder="Filter…"
                value={targetSearch}
                onChange={(e) => setTargetSearch(e.target.value)}
                disabled={targets.length === 0}
              />
            </div>
            {targets.length > 1 && (
              <select className="dash-sort" value={dashSortCol} onChange={(e) => selectDashSort(e.target.value)} title="Sort targets">
                <option value="custom">Custom order</option>
                <option value="status">Status</option>
                <option value="name">Name</option>
                <option value="latency">Latency</option>
                <option value="loss">Loss %</option>
                <option value="jitter">Jitter</option>
                <option value="hops">Hops</option>
              </select>
            )}
          </div>
        </>
      ) : (
        <div className="dashboard-head">
          <SummaryCards
            destSummary={destSummary}
            pathSummary={pathSummary}
            statusFilter={statusFilter}
            toggleStatusFilter={toggleStatusFilter}
          />
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
                {targets.length === 0 ? 'No targets yet' : `${rows.length} of ${targets.length} target${targets.length === 1 ? '' : 's'}`}
              </span>
            </div>
            {targets.length > 0 && (
              <SortBar dashSortCol={dashSortCol} dashSortDir={dashSortDir} toggleDashSort={toggleDashSort} />
            )}
            {targets.length > 0 && (
              <div className="dashboard-actions">
                <button
                  className={'dash-action-pill' + (allPaused ? ' active' : '')}
                  onClick={() => pauseAll(!allPaused)}
                  title="Pause or resume ALL targets"
                >{allPaused ? '▶ Resume all' : '⏸ Pause all'}</button>
                <span className="dashboard-actions-divider" />
                <button className="dash-action-pill" onClick={exportTargetList} title="Save target list + config to a protected .npulse file (only NetPulse can open it)"><IconSave /> Save list (.npulse)</button>
                <button className="dash-action-pill" onClick={exportTargetsJson} title="Export target list + config as human-readable JSON"><IconDataObject /> Export JSON</button>
                <button className="dash-action-pill" onClick={exportAllTargetsFullXlsx} disabled={!!xlsxExportProgress} title="Every target — overview, current hop summary, and full recorded history — one Excel workbook">
                  <IconTableChart /> {xlsxExportProgress ? `Exporting ${xlsxExportProgress.done}/${xlsxExportProgress.total}…` : 'Export all to Excel'}
                </button>
                <button className="dash-action-pill" onClick={onLoadListClick} title="Load a .npulse target list (auto-starts tracing, skips exact duplicates)">
                  <IconUpload /> Load list (.npulse)
                </button>
              </div>
            )}
          </div>
        </div>
      )}

      {targets.length === 0 ? (
        !compact && <EmptyState onQuickAdd={onQuickAdd} onOpenTool={onOpenTool} onLoadListClick={onLoadListClick} />
      ) : (targetSearch.trim() || statusFilter) && rows.length === 0 ? (
        <div className="empty">No targets match the current filter{targetSearch.trim() ? ` "${targetSearch}"` : ''}.{statusFilter && <button className="link-btn" onClick={() => toggleStatusFilter(statusFilter.dim, statusFilter.status)}>Clear status filter</button>}</div>
      ) : (
        <Reorder.Group as="div" axis="y" values={rows} onReorder={onReorder} className="target-panel-list">
          {rows.map((t) => (
            <ReorderableTargetCard
              key={t.id}
              t={t}
              compact={compact}
              isSel={!!(sel && sel.id === t.id)}
              moveTarget={moveTarget}
              forceRecheckOne={forceRecheckOne}
              pauseOne={pauseOne}
              removeTarget={removeTarget}
              openTarget={openTarget}
              targetState={targetState}
              destLamp={destLamp}
              pathLamp={pathLamp}
              destHopOf={destHopOf}
              familyLabel={familyLabel}
              fmt={fmt}
              STATE_LABEL={STATE_LABEL}
              view={view}
              alertMs={alertMs}
            />
          ))}
        </Reorder.Group>
      )}

      {compact && targets.length > 0 && (
        <div className="row">
          <button className={allPaused ? 'primary' : ''} onClick={() => pauseAll(!allPaused)}
            title="Pause or resume ALL targets">{allPaused ? '▶ Resume all' : '⏸ Pause all'}</button>
        </div>
      )}
      {compact && (
        <div className="sidebar-actions">
          <button onClick={exportTargetList} title="Save target list + config to a protected .npulse file (only NetPulse can open it)"><IconSave /> Save list (.npulse)</button>
          <button onClick={exportTargetsJson} title="Export target list + config as human-readable JSON"><IconDataObject /> Export JSON</button>
          <button onClick={exportAllTargetsFullXlsx} disabled={!!xlsxExportProgress} title="Every target — overview, current hop summary, and full recorded history — one Excel workbook">
            <IconTableChart /> {xlsxExportProgress ? `Exporting ${xlsxExportProgress.done}/${xlsxExportProgress.total}…` : 'Export all to Excel'}
          </button>
          <button className="import-btn" onClick={onLoadListClick} title="Load a .npulse target list (auto-starts tracing, skips exact duplicates)">
            <IconUpload /> Load list (.npulse)
          </button>
        </div>
      )}
      {compact && (
        <div
          onMouseDown={startDrag('sidebar')}
          title="Drag to resize"
          className="resize-handle-x absolute top-0 right-0 h-full w-1.5"
        />
      )}
    </div>
  )
}
import { useRef } from 'react'
import StatusLamps from '../StatusLamps'
import MiniVisualPath from '../charts/MiniVisualPath'
import Sparkline from '../charts/Sparkline'
import LatencyGraph from '../charts/LatencyGraph'
import { hopColor, niceCeil } from '../../lib/format'
import { useFlipAnimation } from '../../hooks/useFlipAnimation'

function Metric({ label, value, sub, danger }) {
  return (
    <div className="pstrip-metric">
      <div className="pstrip-metric-label">{label}</div>
      <div className={'pstrip-metric-value' + (danger ? ' danger' : '')}>{value}</div>
      {sub && <div className="pstrip-metric-sub muted">{sub}</div>}
    </div>
  )
}

export default function DashboardTable({
  rows, setDashSortCol, setTargetSort,
  dragHandleProps, dropRowProps, moveTarget,
  forceRecheckOne, pauseOne, removeTarget, openTarget,
  targetState, destLamp, pathLamp, destHopOf, familyLabel, fmt,
  view, alertMs,
}) {
  const listRef = useRef(null)
  // Only re-measure/animate when the actual ORDER of ids changes, not on
  // every ~600ms data refresh (which would re-render this list with the
  // same order but new latency numbers).
  useFlipAnimation(listRef, rows.map((t) => t.id).join(','))

  return (
    <div className="pstrip-list" ref={listRef}>
      {rows.map((t) => {
        const d = destHopOf(t)
        const st = targetState(t)
        const series = (d && t.series) ? (t.series[String(d.hop)] || null) : null
        const rowScaleMax = d ? niceCeil(Math.max(25, d.max ?? d.avg ?? 25)) : 100
        const uptime = t.uptime // { up, pct } — computed by the engine for every target, not just the open one
        return (
          <div
            key={t.id}
            className={'pstrip st-' + st}
            onClick={() => openTarget(t.id)}
            {...dropRowProps(t.id)}
          >
            <span
              className="drag-handle pstrip-handle"
              title="Drag to reorder"
              {...dragHandleProps(t.id, () => openTarget(t.id))}
            >⠿</span>

            <StatusLamps dest={destLamp(t)} path={pathLamp(t)} />

            <div className="pstrip-id">
              <div className="pstrip-name">{t.name}{t.paused && <span className="muted"> · paused</span>}</div>
              <div className="pstrip-dest muted">
                {t.dest_ip || '—'} <span>{familyLabel(t.family, t.config?.family)}</span>
                {t.config && <span className="pstrip-cadence"> · probing every {t.config.probe}s</span>}
              </div>
              {t.error && <div className="pstrip-error">⚠ {t.error}</div>}
              {!t.error && t.loopWarning && <div className="pstrip-error tc-warn-loop" title="Advisory — every hop is still measured normally">⚠ {t.loopWarning}</div>}
            </div>

            <MiniVisualPath hops={t.hops} />

            <div className="pstrip-metrics">
              <Metric
                label="Latency"
                value={d ? `${fmt(d.med ?? d.avg)} ms` : '—'}
                sub={d && d.min != null && d.max != null ? `${fmt(d.min)}–${fmt(d.max)} ms` : null}
              />
              <Metric label="Loss" value={d ? `${d.loss.toFixed(1)}%` : '—'} danger={d && d.loss > 0} />
              <Metric label="Jitter" value={d ? fmt(d.jitter) : '—'} />
              <Metric
                label="Uptime"
                value={uptime ? `${uptime.pct.toFixed(1)}%` : '—'}
                danger={uptime && !uptime.up}
              />
              <Metric label="Hops" value={t.hops?.length || 0} />
            </div>

            {/* Trend/Latency graph — same two header checkboxes that control
                the hop table's Trend/Latency Graph columns also apply here,
                so turning them on shows this target's own recent history
                right in the strip, not just once you open the detail view. */}
            {view.trend && (
              <div className="pstrip-spark" onClick={(e) => e.stopPropagation()}>
                <div className="pstrip-spark-label muted">trend</div>
                <Sparkline points={series || []} color={hopColor(t.id, false)} w={100} h={26} />
              </div>
            )}
            {view.latency && d && (
              <div className="pstrip-latgraph" onClick={(e) => e.stopPropagation()}>
                <div className="pstrip-spark-label muted">latency graph</div>
                <LatencyGraph h={d} prevAvg={null} nextAvg={null} scaleMax={rowScaleMax} alertMs={alertMs} w={140} hgt={26} />
              </div>
            )}

            <div className="pstrip-actions" onClick={(e) => e.stopPropagation()}>
              <button className="card-move" title="Move up" onClick={() => moveTarget(t.id, -1)}>▲</button>
              <button className="card-move" title="Move down" onClick={() => moveTarget(t.id, 1)}>▼</button>
              <button className="card-recheck" title="Force recheck" onClick={() => forceRecheckOne(t)}>↻</button>
              <button className="card-pause" title={t.paused ? 'Resume' : 'Pause'} onClick={() => pauseOne(t)}>{t.paused ? '▶' : '⏸'}</button>
              <button className="card-del" title="Remove" onClick={() => removeTarget(t.id)}>✕</button>
            </div>
          </div>
        )
      })}
    </div>
  )
}
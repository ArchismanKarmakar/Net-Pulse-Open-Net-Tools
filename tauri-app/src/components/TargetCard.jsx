import StatusLamps from './StatusLamps'
import MiniVisualPath from './charts/MiniVisualPath'
import Sparkline from './charts/Sparkline'
import LatencyGraph from './charts/LatencyGraph'
import { hopColor, niceCeil } from '../lib/format'

function Metric({ label, value, unit, sub, danger, tone, hero }) {
  const cls = 'tc-metric-value' + (hero ? ' hero' : '') + (danger ? ' danger' : '') + (tone ? ' tone-' + tone : '')
  return (
    <div className={'tc-metric' + (hero ? ' tc-metric-hero' : '')}>
      <div className="tc-metric-label">{label}</div>
      <div className={cls}>{value}{unit && <span className="tc-metric-unit">{unit}</span>}</div>
      {sub && <div className="tc-metric-sub muted">{sub}</div>}
    </div>
  )
}

export default function TargetCard({
  t, compact, isSel,
  dragControls,
  forceRecheckOne, pauseOne, removeTarget, openTarget,
  targetState, destLamp, pathLamp, destHopOf, familyLabel, fmt, STATE_LABEL,
  view, alertMs,
}) {
  const st = targetState(t)
  const d = destHopOf(t)
  const series = (d && t.series) ? (t.series[String(d.hop)] || null) : null
  const rowScaleMax = d ? niceCeil(Math.max(25, d.max ?? d.avg ?? 25)) : 100

  return (
    <div
      className={'target-card st-' + st + (compact ? ' compact' : '') + (isSel ? ' sel' : '')}
      onClick={() => openTarget(t.id)}
    >
      <div className="tc-row1">
        <span
          className="drag-handle tc-handle"
          title="Drag to reorder"
          onPointerDown={(e) => dragControls.start(e)}
          style={{ touchAction: 'none' }}
        >⠿</span>

        <StatusLamps dest={destLamp(t)} path={pathLamp(t)} />

        <div className="tc-id">
          <div className="tc-name">{t.name}{t.paused && <span className="muted"> · paused</span>}</div>
        </div>

        {/* Mini path topology, then the metric readout — both collapse away
            (width, not just opacity) as the panel narrows. Wrapping each in
            a grid-template-columns 1fr/0fr box is what makes that collapse
            exactly match the real content size instead of guessing a
            max-width, which is what was making the transition look slightly
            off — a size mismatch always shows up as a stutter or a pop at
            the very end of the animation. */}
        <div className="tc-collapse-x">
          <div className="tc-path">
            <MiniVisualPath hops={t.hops} />
          </div>
        </div>

        <div className="tc-collapse-x">
          <div className="tc-full-metrics">
            <span className="tc-vdivider" aria-hidden="true" />
            <div className="tc-metrics">
              {/* Hero stat: the CURRENT (most recent) reading, not the
                  median — this is the number that answers "what is it
                  doing right now", which is what a glance at a dashboard
                  is usually for. Median + range move into the sub-line
                  underneath, still visible but no longer competing for the
                  same visual weight. d.cur can be null on a lost probe
                  (falls back to med/avg with a '*' cue, matching the same
                  convention already used in the detail-panel RTT line). */}
              <Metric
                hero
                label="Latency"
                value={d ? (d.cur != null ? fmt(d.cur) : `${fmt(d.med ?? d.avg)}*`) : '—'}
                unit={d ? ' ms' : null}
                sub={d ? [
                  `med ${fmt(d.med ?? d.avg)} ms`,
                  d.min != null && d.max != null ? `${fmt(d.min)}–${fmt(d.max)} ms` : null,
                ].filter(Boolean).join(' · ') : null}
                tone={st === 'bad' || st === 'down' ? 'bad' : st === 'warn' ? 'warn' : null}
              />
              <span className="tc-metric-divider" aria-hidden="true" />
              <Metric label="Loss" value={d ? `${d.loss.toFixed(1)}%` : '—'} danger={d && d.loss > 0} />
              <Metric label="Jitter" value={d ? fmt(d.jitter) : '—'} />
              <Metric
                label="Uptime"
                value={t.uptime ? `${t.uptime.pct.toFixed(1)}%` : '—'}
                danger={t.uptime && !t.uptime.up}
              />
              <Metric label="Hops" value={t.hops?.length || 0} />
            </div>
            {view.trend && (
              <>
                <span className="tc-metric-divider" aria-hidden="true" />
                <div className="tc-spark" onClick={(e) => e.stopPropagation()}>
                  <div className="tc-spark-label muted">trend</div>
                  <Sparkline points={series || []} color={hopColor(t.id, false)} w={100} h={26} />
                </div>
              </>
            )}
            {view.latency && d && (
              <div className="tc-latgraph" onClick={(e) => e.stopPropagation()}>
                <div className="tc-spark-label muted">latency graph</div>
                <LatencyGraph h={d} prevAvg={null} nextAvg={null} scaleMax={rowScaleMax} alertMs={alertMs} w={140} hgt={26} />
              </div>
            )}
          </div>
        </div>

        {STATE_LABEL[st] && <span className={'statelabel status-pill st-' + st}>{STATE_LABEL[st]}</span>}

        <div className="tc-actions" onClick={(e) => e.stopPropagation()}>
          <button className="card-recheck" title="Force recheck" onClick={() => forceRecheckOne(t)}>↻</button>
          <button className="card-pause" title={t.paused ? 'Resume' : 'Pause'} onClick={() => pauseOne(t)}>{t.paused ? '▶' : '⏸'}</button>
          <button className="card-move" title="Move up" onClick={() => moveTarget(t.id, -1)}>▲</button>
          <button className="card-move" title="Move down" onClick={() => moveTarget(t.id, 1)}>▼</button>
          <button className="card-del" title="Remove" onClick={() => removeTarget(t.id, isSel)}>✕</button>
        </div>
      </div>

      {/* Deliberately NOT nested inside .tc-id any more — .tc-dest used to
          stack under .tc-name and inherit whatever x-position the name
          column happened to start at, which drifted with font metrics and
          with the .sel extra-indent. Pinning its indent to the fixed
          handle-width + row-gap instead makes it start directly under the
          status lamps, and keeps it there in both compact and dashboard
          mode and regardless of selection, since all three are constants,
          not measurements of where something else landed. */}
      <div className="tc-subrow">
        <div className="tc-dest muted">
          {t.dest_ip || '—'}<span>{familyLabel(t.family, t.config?.family)}</span>
          <span className="tc-cadence"> · probing every {t.config?.probe}s</span>
        </div>
        {t.error && <div className="tc-error">⚠ {t.error}</div>}
        {!t.error && t.loopWarning && <div className="tc-error tc-warn-loop" title="Advisory — every hop is still measured normally">⚠ {t.loopWarning}</div>}
      </div>

      {/* min/avg/med/max/jit/pl readout — the compact sidebar's own tight
          format, collapses away entirely in dashboard mode (see
          tc-collapse-y in styles.css). Dashboard shows the fuller labeled
          Latency/Loss/Jitter/Uptime/Hops row above instead. */}
      {d && (
        <div className="tc-collapse-y">
          <div className="tc-compact-grid">
            <span>min <b>{fmt(d.min)}</b></span>
            <span>avg <b>{fmt(d.avg)}</b></span>
            <span>med <b>{fmt(d.med)}</b></span>
            <span>max <b>{fmt(d.max)}</b></span>
            <span>jit <b>{fmt(d.jitter)}</b></span>
            <span>pl <b className={d.loss > 0 ? 'danger' : ''}>{d.loss.toFixed(0)}%</b></span>
          </div>
        </div>
      )}
    </div>
  )
}
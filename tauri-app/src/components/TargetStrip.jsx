import React from 'react';

// Minimal inline SVGs to guarantee they load without extra library dependencies
const Icons = {
  Drag: () => <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="9" cy="12" r="1"/><circle cx="9" cy="5" r="1"/><circle cx="9" cy="19" r="1"/><circle cx="15" cy="12" r="1"/><circle cx="15" cy="5" r="1"/><circle cx="15" cy="19" r="1"/></svg>,
  Monitor: () => <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><rect x="2" y="3" width="20" height="14" rx="2" ry="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>,
  Server: () => <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><rect x="2" y="2" width="20" height="8" rx="2" ry="2"/><rect x="2" y="14" width="20" height="8" rx="2" ry="2"/><line x1="6" y1="6" x2="6.01" y2="6"/><line x1="6" y1="18" x2="6.01" y2="18"/></svg>,
  Refresh: () => <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M21 2v6h-6"/><path d="M3 12a9 9 0 0 1 15-6.7L21 8"/><path d="M3 22v-6h6"/><path d="M21 12a9 9 0 0 1-15 6.7L3 16"/></svg>,
  Pause: () => <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><rect x="6" y="4" width="4" height="16"/><rect x="14" y="4" width="4" height="16"/></svg>,
  Play: () => <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polygon points="5 3 19 12 5 21 5 3"/></svg>,
  Up: () => <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polyline points="18 15 12 9 6 15"/></svg>,
  Down: () => <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polyline points="6 9 12 15 18 9"/></svg>,
  Close: () => <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
};

export default function TargetStrip({ 
  target, isActive, onClick, onClose, onPause, onForceRecheck, onMoveUp, onMoveDown, provided 
}) {
  // 1. Data Extraction
  const hops = target.latest?.hops || [];
  const destHop = hops.find(h => h.is_dest) || hops[hops.length - 1];
  
  const latency = destHop?.avg != null ? destHop.avg.toFixed(1) : '-';
  const min = destHop?.min != null ? destHop.min.toFixed(1) : '-';
  const max = destHop?.max != null ? destHop.max.toFixed(1) : '-';
  const loss = destHop?.loss != null ? destHop.loss.toFixed(1) : '0.0';
  const jitter = destHop?.jitter != null ? destHop.jitter.toFixed(1) : '-';
  const uptime = target.uptime?.pct != null ? target.uptime.pct.toFixed(1) : '100.0';
  const hopCount = hops.length;
  
  const family = target.config?.family === 'v4' ? 'IPv4' : target.config?.family === 'v6' ? 'IPv6' : 'Auto';
  const probeInt = target.config?.probe || 1;

  // 2. Status Color Logic
  let statusColor = '#00ff41'; // Healthy Green
  let statusText = 'ok';
  
  if (destHop?.loss >= 100 || !target.uptime?.up) {
    statusColor = '#ff3030'; // Down Red
    statusText = 'down';
  } else if (destHop?.loss > 0 || destHop?.avg > 150) {
    statusColor = '#ffb000'; // Warning Amber
    statusText = 'warning';
  } else if (hops.some(h => h.loss > 0)) {
    statusColor = '#ffb000'; // Path Warning
    statusText = 'path';
  }

  // 3. Mini Visual Path Generator
  const renderVisualPath = () => {
    if (hops.length === 0) return <div className="mvp-empty">Discovering...</div>;

    const firstHop = hops[0];
    const lastHop = hops[hops.length - 1];
    // Find highest latency/loss intermediate hop to feature it
    const midHop = hops.slice(1, -1).reduce((prev, current) => 
      (prev && prev.loss > current.loss) ? prev : current, null
    );

    const HopNode = ({ h, isGray }) => (
      <div className={`mvp-node ${h.loss >= 100 ? 'down' : h.loss > 0 || h.avg > 150 ? 'warn' : 'ok'} ${isGray ? 'gray' : ''}`}>
        {h.hop}
      </div>
    );

    return (
      <div className="mini-visual-path">
        <Icons.Monitor />
        <div className="mvp-line" />
        <HopNode h={firstHop} />
        
        {hops.length > 2 && (
          <>
            <div className="mvp-line dashed" />
            {midHop && midHop.hop !== firstHop.hop && midHop.hop !== lastHop.hop && (
              <>
                <HopNode h={midHop} isGray={true} />
                <div className="mvp-line dashed" />
              </>
            )}
          </>
        )}
        
        {hops.length > 1 && (
          <>
            {!midHop && hops.length > 2 && <div className="mvp-line" />}
            <HopNode h={lastHop} />
          </>
        )}
        
        <div className="mvp-line" />
        <div style={{ color: statusColor }}><Icons.Server /></div>
      </div>
    );
  };

  // 4. Sparkline SVG Generator
  const renderSparkline = () => {
    const series = target.series?.[destHop?.hop] || [];
    if (series.length === 0) return null;
    
    // Simplistic downsample for sparkline path
    const pts = series.slice(-40); 
    const maxVal = Math.max(...pts.map(p => p[1] || 0), 10);
    const minVal = Math.min(...pts.filter(p => p[1] != null).map(p => p[1]));
    
    const pathD = pts.map((p, i) => {
      const x = (i / (pts.length - 1)) * 60;
      const y = p[1] == null ? 20 : 20 - (((p[1] - minVal) / (maxVal - minVal || 1)) * 18);
      return `${i === 0 ? 'M' : 'L'} ${x} ${y}`;
    }).join(' ');

    return (
      <svg className="sparkline-svg" width="60" height="20" viewBox="0 0 60 20">
        <path d={pathD} fill="none" stroke={statusColor} strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
      </svg>
    );
  };

  return (
    <div 
      className={`target-strip-wide ${isActive ? 'active' : ''} ${target.paused ? 'paused' : ''}`}
      style={{ borderLeftColor: statusColor, ...(provided?.draggableProps?.style || {}) }}
      ref={provided?.innerRef}
      {...provided?.draggableProps}
      onClick={onClick}
    >
      {/* LEFT: Identifiers */}
      <div className="ts-left">
        <div className="ts-drag" {...provided?.dragHandleProps}><Icons.Drag /></div>
        <div className="ts-status-blocks">
            <span style={{ background: statusColor }}></span>
            <span style={{ background: hops.some(h => h.loss > 0 && h.hop !== destHop?.hop) ? '#ffb000' : statusColor }}></span>
        </div>
        <div className="ts-titles">
          <div className="ts-name">{target.target}</div>
          <div className="ts-sub">{target.latest?.dest_ip || 'Resolving...'} {family} (auto) · probing every {probeInt}s</div>
        </div>
      </div>

      {/* MIDDLE: Visual Path */}
      <div className="ts-middle">
        {renderVisualPath()}
      </div>

      {/* RIGHT: Metrics Grid */}
      <div className="ts-metrics">
        <div className="ts-metric">
          <span className="ts-label">LATENCY</span>
          <span className="ts-val">{latency} <small>ms</small></span>
          <span className="ts-sub-val">{min}-{max} ms</span>
        </div>
        <div className="ts-metric">
          <span className="ts-label">LOSS</span>
          <span className="ts-val">{loss}%</span>
        </div>
        <div className="ts-metric">
          <span className="ts-label">JITTER</span>
          <span className="ts-val">{jitter}</span>
        </div>
        <div className="ts-metric">
          <span className="ts-label">UPTIME</span>
          <span className="ts-val">{uptime}%</span>
        </div>
        <div className="ts-metric">
          <span className="ts-label">HOPS</span>
          <span className="ts-val">{hopCount}</span>
        </div>
      </div>

      {/* FAR RIGHT: Trend & Actions */}
      <div className="ts-actions-group">
        <div className="ts-trend">
          <span className="ts-label">trend</span>
          {renderSparkline()}
        </div>
        <div className="ts-status-text" style={{ color: statusColor }}>{statusText}</div>
        
        <div className="ts-btn-row">
          <button className="ts-btn" onClick={(e) => { e.stopPropagation(); onForceRecheck(target.id); }} title="Force Recheck"><Icons.Refresh /></button>
          <button className="ts-btn" onClick={(e) => { e.stopPropagation(); onPause(target.id, !target.paused); }} title={target.paused ? "Resume" : "Pause"}>
            {target.paused ? <Icons.Play /> : <Icons.Pause />}
          </button>
          <button className="ts-btn" onClick={(e) => { e.stopPropagation(); onMoveUp(); }}><Icons.Up /></button>
          <button className="ts-btn" onClick={(e) => { e.stopPropagation(); onMoveDown(); }}><Icons.Down /></button>
          <button className="ts-btn close-btn" onClick={(e) => { e.stopPropagation(); onClose(target.id); }}><Icons.Close /></button>
        </div>
      </div>
    </div>
  );
}
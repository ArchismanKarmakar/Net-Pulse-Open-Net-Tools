// German-Hauptsignal-style two-lamp status: left lamp is the DESTINATION's
// own health, right lamp is the PATH (intermediate hops) leading to it.
// This is the app's one canonical status indicator — used in the sidebar
// strip and the dashboard performance strips alike, so a target reads the
// same way everywhere instead of the dashboard inventing its own separate
// "ring" convention.
const DEST_TITLES = {
  discovering: 'Route discovery in progress',
  settling: 'Destination healthy — latency/route changed recently, stabilising',
  ok: 'Destination healthy',
  okloss: 'Destination: occasional loss',
  warn: 'Destination: elevated latency or minor loss (70ms+ / PL>5%)',
  bad: 'Destination: high latency or loss (100ms+ / PL>10%)',
  down: 'Destination: unreachable, or severe latency/loss/jitter (150ms+ / PL>20% / 50ms+ swings)',
}
const PATH_TITLES = {
  discovering: 'Route discovery in progress',
  ok: 'Path healthy — packets routing cleanly to the target',
  warn: 'Path: an intermediate hop is dropping packets or not revealing itself',
  bad: 'Path: routing to the target pool via BGP, but the target or a router is dropping packets',
  down: 'Path: no route — internet/interface down, RTO, or first hop rejecting',
}

export default function StatusLamps({ dest, path }) {
  return (
    <span className="signal">
      <span
        className={'lamp st-' + dest}
        title={`Target: ${DEST_TITLES[dest] || dest}`}
        aria-label={`target-${dest}`}
      />
      <span
        className={'lamp st-' + path}
        title={`Path: ${PATH_TITLES[path] || path}`}
        aria-label={`path-${path}`}
      />
    </span>
  )
}

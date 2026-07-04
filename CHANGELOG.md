# Changelog

## Two-lamp status signal + documentation

### Added — full two-lamp status system (`web/src/App.jsx`, `web/src/styles.css`)

Implemented the target/path two-lamp signal per the project's lamp diagram.
Each sidebar target shows two independent lamps: **left = target** (destination
health), **right = path** (route health).

- **`destLamp(t)`** — target health, graduated exactly per the diagram via a new
  `LAMP` threshold object:
  - green (`ok`): healthy.
  - lime (`settling`, pulsing): healthy but route/latency changed recently —
    held until stable via a timer (`LAMP.settleSecs`), detected by watching the
    destination hop's address and median RTT for material changes
    (`routeSettle` ref + `targetSettling`).
  - yellow (`warn`): latency ≥ 70 ms or loss > 5%.
  - orange (`bad`): latency ≥ 100 ms or loss > 10%.
  - red (`down`): unreachable, or latency ≥ 150 ms, or loss > 20%, or jitter
    ≥ 50 ms.
- **`pathLamp(t)`** — route health:
  - green (`ok`): clean routing.
  - yellow (`warn`): an intermediate hop dropping packets or not revealing
    itself (`*`/silent router).
  - orange (`bad`): routing to the target pool but target/router dropping
    packets.
  - red (`down`): no route (internet/interface down, RTO, first-hop rejecting).
- Thresholds in `LAMP` are intentionally separate from the `alerts.ms/loss`
  pair (which still drives table highlighting and the alert banner), because the
  diagram defines a graduated 70/100/150 ms · 5/10/20% ladder a single pair
  can't express.
- New CSS: `--lime` color token (dark `#a3e635`, light `#65a30d`);
  `.lamp.st-settling`, `.statelabel.st-settling` (lime, gentle pulse).
- `web/tailwind.config.js`: added `s-settling` / `st-settling` to the `safelist`
  (runtime-composed classes are otherwise tree-shaken out — see maintainer note
  in README).
- Lamp tooltips updated to describe each state in the diagram's own terms.

Verified: target-lamp thresholds checked against 9 synthetic cases matching each
diagram row (all pass); frontend builds clean; new lamp CSS confirmed present in
the compiled bundle.

### Docs

- `README.md`: new **Status lamps** section (both lamp tables + threshold
  location + Tailwind safelist maintainer note) and **Route discovery** section
  (documents the per-hop cadence + global token-bucket rate limiter, the
  stale-reply rejection, and NAT/load-balancer EchoReply handling). Intro
  updated to point at the lamps.
- `THIRD_PARTY_NOTICES.md`: reviewed — unchanged (no dependency changes this
  session).

### Not changed

The engine/discovery rewrite, the strict `is_dest` destination logic, and the
discovery-drop grace window were authored upstream (before this session); they
were used as the baseline and are documented in the README, not modified here.
The only behavioural code added this session is the lamp logic in the renderer.

# Changelog

## Unreleased

- **Tools are now real tabbed pages** (top tab bar): **Path / MTR** (home, default),
  **Ping** (streams the OS `ping`, cmd-style), **DNS Lookup** (forward A/AAAA/CNAME +
  reverse PTR), and **Port Scanner** (bounded TCP connect, ≤2048 ports/scan). Backends
  are implemented in the Electron main process (Node dns/net/child_process) over IPC.
- **License changed to AGPL-3.0-or-later** (was MIT) to keep the project copyleft /
  open-source for community development, VLC-style. Added `LICENSE` (full AGPL text),
  `COPYRIGHT`, updated package manifests and README.
- **IPv6 startup-loss:** reduced the discovery window (kDiscoveryWindow 6→3) so the
  destination is hit by a smaller echo burst during route discovery, and the build now
  does a **clean addon rebuild** (`cmake-js rebuild`, not incremental `compile`) — an
  incremental build could silently skip a header-only engine change, which is the most
  likely reason earlier fixes appeared to have no effect. The build also now fails hard
  if the .node addon isn't produced, and the engine prints its build tag on load.


## Unreleased

- **IPv6 startup-loss fix (round 2 — root cause + verifiability).** In addition to
  holding a hop's early losses during settling, the destination hop's discovery-phase
  samples are now discarded the moment the destination is confirmed. During discovery
  every TTL at/beyond the destination reaches it, and IPv6 anycast endpoints
  (Cloudflare/Google) rate-limit that echo burst far harder than IPv4 — so the dest's
  first samples were burst-induced losses. Once the destination is known the fan-out
  stops and probing is one packet/interval, so we let the dest measure fresh from that
  point. Verified in simulation for both fast and slow confirmation (0 loss exposed).
- **Engine build banner.** The native addon now prints `[Net Pulse] native engine
  loaded — build <tag>` on load and exports `engineBuild`, so you can confirm the
  .node addon was actually recompiled (restarting Electron alone keeps the old binary).


## Unreleased

- **Fix — IPv6 hostname targets started at 100% loss for 3–5 s, then recovered
  (IPv4 was unaffected):** the per-hop discovery "settling" window suppressed a
  hop's first few *replies* but recorded its *losses* immediately. IPv6 paths
  rate-limit the initial probe burst that discovers the route, so a hop's first
  probes are dropped by the network — those losses showed at 100% while the
  genuine early replies were still held back, which is why IPv6 flashed loss at
  startup and IPv4 (whose initial probes weren't dropped) did not. A hop's early
  losses are now held for the same settling window (until it has produced
  kDiscoveryDropCount replies or the grace window elapses), so both families
  show a brief "discovering" state and then real data. A genuinely unreachable
  hop still surfaces its 100% loss a few seconds in.


## Unreleased

- **Fix — stuck on "discovering" after a reconnect / route change:** the frontier
  (`max_hop_seen_`) now *decays* to the deepest hop that answered recently instead of
  the deepest that ever answered, and phantom rows (hops that got an address from a
  stray reply during an outage but are now silent and beyond the frontier) are pruned.
  This lets a target settle back to its true state (path / unreachable) after the link
  is restored, instead of holding a sent=0 ghost row and spinning forever.
- **Payload up to 65500 B** (was 1472), matching `ping -l`; larger-than-MTU sizes are
  OS-fragmented. All config fields (probe, trace, timeout, payload, max hops) now carry
  min/max limits and are validated on Add and on live Edit, with inline errors.
- **Menu bar** (File / Targets / View / Tools / Help) with quick-trace shortcuts,
  pause-all, view toggles, config edit, and About.
- **Pause reworked:** per-target pause on every card (⏸/▶) plus a global Pause-all /
  Resume-all. The **Stop** button (which only froze a target with no way to resume) was
  removed — use pause to freeze, ✕ to remove.


## Unreleased

- **Rebrand:** renamed to **Net Pulse — Open Net Tools** across the window title, in-app header, HTML title, package manifests, VS Code tasks and README; added a new logo (`branding/logo.svg`) wired in as the app/window icon, favicon and header mark.
- **Fix (link loss / route flux, e.g. ISP restart):** ICMP *Destination Unreachable* replies are no longer recorded as transit hops. Previously an Unreachable from your gateway/CGNAT during an outage was painted as a hop at the probe's TTL, scattering private/CGNAT IPs (192.168.x, 10.x) across random high hops. They are now counted as loss for that hop; only *Time Exceeded* (a genuine transit hop) and *Echo Reply* / destination-sourced Unreachable (arrival) populate the path.


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

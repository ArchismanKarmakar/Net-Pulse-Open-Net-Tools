# Changelog


## 0.8.0 — direct-echo measurement, global pacer, shared-hop pub/sub, DNS pool

- **Direct-echo hop measurement (fixes "healthy hop shows heavy loss under
  traceroute").** Measuring a hop by eliciting its ICMP Time-Exceeded (the
  traceroute way) measures the wrong thing: generating a Time-Exceeded is
  control-plane work that every router rate-limits hard, so a hop that
  answers a standalone `ping` at 0%% loss can read 30-100%% loss purely from
  that rate limit — while the destination, reached with a full-TTL echo and
  answered with a (non-rate-limited) Echo Reply, stays clean. Once a hop's
  IP is discovered, the engine now pings that IP directly (`TTL=255`,
  exactly what `ping <hop-ip>` does) instead of continuing to elicit its
  Time-Exceeded. A hop that never answers a direct ping after 4 tries is
  marked echo-silent and falls back to legacy probing (its rate-limit loss
  is then real and unavoidable, same as `mtr`/`tracert` would show). A rare
  (45s) legacy re-probe still runs per hop so a mid-session route change is
  never missed just because direct-echo stopped eliciting Time-Exceeded.
- **Global send pacer.** A per-target token bucket alone bounds one target;
  with N targets the *aggregate* rate onto a hop they all share is N times
  that, which blows past a router's own ICMP rate limit long before N gets
  large. Added a process-global token bucket that every send must also
  satisfy — the rate scales with active target count but is hard-capped, so
  the aggregate can never exceed a safe ceiling no matter how many targets
  run concurrently.
- **Shared-hop pub/sub cache.** All targets on the same egress traverse the
  same early hops; there's no reason for 100 targets to each probe the
  router once per interval. Whichever session already has a fresh real
  reply for a given hop publishes it; every other session adopts that
  sample and skips its own send for that round. Gated to public IPs only —
  a private/CGNAT address is only unambiguous within one routing domain, so
  two targets could otherwise attribute one physical device's RTT to a
  completely different device behind a different NAT/VRF boundary.
- **6-worker reverse-DNS pool.** Reverse DNS now runs on 6 background
  workers against a shared, deduplicated queue/cache instead of one, so a
  single slow/hanging PTR lookup (a node that lets a query sit until its own
  multi-second timeout) can no longer stall every other pending hostname
  behind it. Also now resolves LAN/private hops, not just public ones, so a
  home router's own PTR record (e.g. `RT-XXXX.home.arpa`) is shown — the
  gap that started this whole investigation.

## 0.7.6 — fix multi-target shared-hop loss (reply misdirection)

- **Root cause found:** with multiple targets, each runs its own raw ICMP socket, but the OS
  (notably Windows) often delivers ALL inbound ICMP to just ONE of those sockets. A reply for
  target B arriving on target A's socket was discarded (wrong ICMP id), so B never saw its
  router/BNG replies — the same shared hop showed 0%% loss for one target and ~90%% for another
  (your 1.1.1.1 vs 8.8.8.8 on 10.1.1.1). This is why it appeared with the multi-threaded design.
- **Fix:** a process-global registry maps each session's unique ICMP id -> session. A session
  that receives a reply not addressed to it now ROUTES it to the owning session's inbox, which
  that session drains on its own thread. So every reply reaches the right session no matter
  which socket the OS delivered it to. Duplicates (when the OS does copy to all sockets) are
  harmlessly de-duplicated by the pending-map. Verified with a concurrent test: with 100%% of
  replies misdirected to one socket, both targets still record 100%% of their replies.
- Reverted the 0.7.5 per-IP rate cap — that addressed rate-limiting, but the real issue was
  reply misdirection; the engine's send path is back to the v19 behaviour plus this routing.


## 0.7.5 — cross-target shared-hop rate coordination

- **Fixed multi-target loss on shared hops (router / BNG / common intermediates).** Root
  cause: each target runs on its own thread + socket (correct), but those threads probed the
  SAME shared hops independently, so N targets put N× the ICMP load on one device and the
  router's ICMP rate-limiting dropped the excess — loss that grows with target count (present
  in v19 too; it's inherent to N uncoordinated traceroutes through one router).
  Added a process-global token bucket keyed by hop IP: the AGGREGATE probe rate to any single
  hop IP across all target threads is capped (~3/s). It applies ONLY to already-discovered
  hops — route discovery is never throttled — and a throttled probe is skipped, NOT counted
  as loss. Verified: 1 target unaffected (10/10 sent); 6 targets to one router capped at ~3/s
  aggregate; unique destinations never throttled.
- The multi-threaded design itself is sound (independent thread + socket + unique ICMP id per
  target); the missing piece was cross-thread coordination on shared hops, now added.


## 0.7.4 — restore v19 hop discovery

- **Fixed 'not all hops discovered' regression** by reverting the ICMP-identifier change: the
  engine's probing/discovery code is now byte-identical to the known-good v19 (the only
  additions are the no-op-when-empty per-hop-pause skip and the pausedHops config field).
  v19's id formula already makes each target's id per-target-unique, so replies still can't
  be stolen between targets — the extra atomic-counter change was unnecessary and was the
  sole engine difference from v19, so it's gone.
- **Multi-target shared-hop loss:** with the engine back to v19, this returns to v19's level.
  The residual loss when several targets trace through the same router is the router's own
  ICMP-error (TTL-exceeded) rate-limiting — inherent to concurrent traceroute through one
  device, not an app bug; pinging the router directly (echo reply, not rate-limited) stays clean.
- UI fixes from 0.7.3 retained: solid chart hover tooltip, accent-coloured dark-mode selection
  bar, and the advanced Ping tool.


## 0.7.3 — multi-target ICMP fix, tooltip/selection UI, advanced ping

- **Multi-target router loss:** each target session now gets a process-GLOBAL unique ICMP
  identifier (atomic counter) instead of a time-based one. Every raw ICMP socket receives a
  copy of every reply, so a colliding id let one target match/consume another's shared-hop
  (router) replies — exactly the 'enable target 1 → target 2 router goes 100%%' symptom.
  Unique ids mean replies are attributed to exactly one session.
  NOTE: routers (incl. ASUS) rate-limit ICMP *error* (TTL-exceeded) generation per RFC, so
  many targets tracing through the same router can still show some shared-hop loss — that
  part is the router, not the app; pinging the router directly (echo reply, not rate-limited)
  won't show it.
- **Confirmed multi-threaded:** every target runs on its own std::thread with its own raw
  socket and unique ICMP id; the resolver runs in the Electron main process. Independent and
  concurrent.
- **Chart hover tooltip** now has a solid themed background (was transparent → text blended
  into the plot grid in both themes).
- **Selected-target bar** is now accent-coloured in dark mode (the accent tokens existed only
  for light mode, so the dark bar rendered black).
- **Advanced Ping tool:** size, timeout, TTL, interval, IPv4/IPv6, and continuous mode
  (cross-platform flag mapping), live parsed stats (sent/recv/loss/min/avg/max/jitter), and
  colorized output.


## 0.7.2 — latest packages, everything working

- **All packages at latest, probing preserved.** The 0-hops regression was proven (by the
  v19 diff) to be the in-engine resolver thread producing empty hops — the frontend parsed
  dest/config fine, so React 19 / Vite 8 were NOT the cause. The engine stays on v19's
  byte-identical probing core (no resolver thread; hostnames run in the Electron main
  process instead), so we can ship the latest frontend safely:
  - React **19.2**, Vite **8.1** (Rolldown), @vitejs/plugin-react **6**, Recharts **3.9**,
    Tailwind 3.4 (latest 3.x; v4 is a breaking config rewrite, not requested).
  - Electron **43**, electron-builder **26**, node-addon-api **8.5**, cmake-js **8.0**, C++**20**.
- **Security: 0 vulnerabilities across every tree** (web, napi, electron). cmake-js 8 drops
  the vulnerable tar + deprecated npmlog/gauge/are-we-there-yet; glob pinned to **13.0.6**
  (current, not deprecated); rimraf 6 + inflight/boolean stubs. Only unavoidable dev-only
  note is any transitive glob the toolchain still resolves — now on 13, it's clean.
- Per-hop pause, tabbed tools, dark-mode sidebar isolation, and main-process hostnames retained.


## 0.7.1 — restore working probing (regression fix) + cmake-js 8 + glob 13

- **Fixed: all targets stuck "discovering / 0 hops".** Root cause isolated by diffing
  against the known-good v19: the regression was the in-engine reverse-DNS **resolver
  thread** plus the **React 19 / Vite 8 (Rolldown)** frontend — v19 uses React 18 + Vite 6
  and has no engine thread. The engine is restored to v19's byte-identical probing core
  (only the safe, no-op-when-empty per-hop-pause skip is re-added), and the frontend is
  back on the verified React 18.3 + Vite 6.3 stack (Recharts 3 kept — v19 already used it).
- **Hostnames without touching the engine:** reverse DNS now runs in the Electron main
  process (Node `dns`) and fills the HOST column via a cached lookup — no getnameinfo on
  the probe path, so it can't stall or crash probing.
- **cmake-js 8.0** (was 7.4): removes the vulnerable `tar` and the deprecated
  npmlog/gauge/are-we-there-yet stack. napi now audits **0 vulnerabilities**, no deprecations.
- **glob 13.0.6** override (was forcing 11): 13 is the current release and is **not**
  deprecated, so the electron packaging tree no longer shows the glob deprecation. rimraf 6
  + inflight/boolean stubs remain → **0 vulnerabilities**.
- Per-hop pause, tabbed tools, and dark-mode sidebar isolation retained.


## 0.7.0 — package refresh, hostnames, per-hop pause

- **Latest toolchain:** React **19**, Vite **8** (Rolldown), Recharts **3**, Electron **43**,
  node-addon-api **8.9**, C++**20**. Web build verified; web tree has **0 vulnerabilities**.
  electron-builder kept at 26 with **npm `overrides`** (modern glob/rimraf, stubbed
  inflight/boolean) → **0 vulnerabilities** and all deprecations removed EXCEPT `glob`,
  which its own maintainer marks deprecated on *every* version (a funding notice) and which
  electron-builder must pull — so it is unavoidable, dev-only, and non-vulnerable. Tailwind
  kept at 3.4 (latest 3.x, not deprecated, no vulns): v4 is a config-format rewrite that
  would risk 100+ @apply/theme calls with zero security benefit.
- **Hostnames now resolve** (HOST column). Reverse DNS runs on a dedicated per-session
  **resolver thread** with a request/result queue, so getnameinfo never stalls the probe
  loop — this also directly advances the multi-threading goal (probe thread + resolver
  thread per target, all sessions independent).
- **Per-hop pause:** each hop row has a ⏸/▶ toggle to stop probing that hop (cuts network
  load); paused hops are skipped in the send loop and reported in the target config.
- **Dark-mode sidebar isolation:** target cards now have a raised fill + border per theme
  so the list reads as distinct cards in dark mode, matching light mode.
- **N-API safety:** pausedHops input is validated/bounded like all other numeric input.


## Unreleased

- **Definitive fix for `node-gyp` running + `/std:c++20 → c++17` downgrade.** Root cause
  was a **stale `napi/binding.gyp`** left behind when a new release is unzipped *over* an
  old folder — npm then auto-runs node-gyp (which forces C++17) instead of the CMake.js
  C++20 build. `binding.gyp` is gone from the project, and now a no-op `install` script in
  `napi/package.json` means npm will **never** auto-run node-gyp even if a stale
  `binding.gyp` is present; `build-and-run` also deletes any stale `napi/binding.gyp` +
  `napi/build/` before building. README warns to extract into a clean folder.
- Clarified that the `inflight`/`glob@7`/`rimraf@2`/`boolean` deprecation warnings are
  **electron-builder's dev-only transitive deps** — not shipped in the app, not a runtime
  security concern; a clean reinstall (no stale tree) also drops stray packages like
  electron-winstaller.


## Unreleased

- **N-API hardening:** every exported native function is now exception-safe — the
  pause/stop/remove/listInterfaces entry points gained try/catch so a C++ exception can
  never cross the N-API boundary and abort the process (add/update already had it).
  Numeric options now reject NaN/Inf, and `target` must be a proper non-empty string
  (was coerced, which could add a literal "undefined" target).
- **Deployment trust:** confirmed the signing-ready `electron/electron-builder.yml`
  (Authenticode via CSC_LINK/CSC_KEY_PASSWORD, SHA-256 + RFC-3161 timestamp, macOS
  hardened-runtime + entitlements, no UPX/packer) and the SECURITY.md guide covering
  code-signing, SmartScreen reputation, and clearing VirusTotal detections. Honest
  caveat documented: an unsigned network tool with a port scanner will draw heuristic
  flags regardless of code cleanliness — signing + reputation is the fix.


## Unreleased

- **Security hardening & trusted-release pipeline:**
  - N-API boundary: `pauseTarget`/`stopTarget`/`removeTarget` now validate arity/type
    and throw cleanly (all inputs were already range-clamped and exception-wrapped).
  - Electron: explicit `webSecurity`/`allowRunningInsecureContent:false`/`webviewTag:false`,
    and a deny-all permission request/check handler (on top of existing contextIsolation,
    sandbox, CSP, and navigation locks).
  - Added **electron-builder** config (`electron/electron-builder.yml`) producing NSIS /
    dmg+zip / AppImage+deb, with **code-signing + macOS notarization wired via env vars**
    (no secrets in the repo), correct resource layout for the prebuilt UI and `.node`
    addon, and no packing/obfuscation. Added `dist*` scripts and installer icons.
  - Added **SECURITY.md** — hardening posture and an honest, actionable guide to making
    signed builds trusted by SmartScreen/AV/VirusTotal (signing, notarization, reputation,
    false-positive handling).


## Unreleased

- **Toolchain modernization & build-compat fixes:**
  - `napi/index.js` now forwards the engine build tag, so `engine build: <tag>` shows
    the real value instead of a false `unknown` / "OLD addon" warning.
  - Removed `binding.gyp`: the addon builds with **CMake.js only**. `npm i` no longer
    auto-runs **node-gyp** and no longer produces a wrong-ABI `.node` that shadowed the
    CMake.js build.
  - `build:electron` now **auto-detects the installed Electron version** (build-electron.js)
    instead of hard-coding 29.1.0 — upgrading Electron no longer causes an ABI mismatch.
  - Both addon build scripts use clean `cmake-js rebuild` (also in the VS Code task).
  - **C++20** (was C++17) across both CMake targets — clears the MSVC `/std:c++20`
    override warning and modernizes the build.
  - Dependency bumps: Electron 29 → **^33**, Vite 5 → **^6.3**, Recharts 2.12 → **^2.15**,
    node-addon-api → **^8.5**; added `engines: node >=20` (active LTS). React 18 and
    Tailwind 3 kept intentionally (React 19 needs a Recharts-3 migration; Tailwind 4 is a
    config rewrite) to avoid breaking the UI. Web build verified on Vite 6.


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

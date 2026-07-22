# Net Pulse — Open Net Tools

<p align="center"><img src="branding/logo.svg" width="96" height="96" alt="Net Pulse — Open Net Tools logo" /></p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0--or--later-22d3ee" alt="AGPL-3.0-or-later" /></a>
  <img src="https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux-4f7cff" alt="Platforms" />
  <img src="https://img.shields.io/badge/telemetry-none-38e0d6" alt="No telemetry" />
  <a href="https://github.com/sponsors/ArchismanKarmakar"><img src="https://img.shields.io/badge/sponsor-%E2%99%A5-ff69b4" alt="Sponsor on GitHub" /></a>
</p>

<p align="center">
  <a href="https://archismankarmakar.github.io/Net-Pulse-Open-Net-Tools/">Project site</a> ·
  <a href="ARCHITECTURE.md">Architecture</a> ·
  <a href="SECURITY.md">Security</a> ·
  <a href="PRIVACY_POLICY.md">Privacy policy</a> ·
  <a href="https://github.com/sponsors/ArchismanKarmakar">Sponsor</a>
</p>

> ### 🦀 Now on Tauri + Rust + React (the C++ engine is unchanged)
> The desktop shell moved from Electron/Node-API to **Tauri + Rust**, calling
> the *same* C++ probe engine through a `cxx` bridge — statically linked into
> the Rust binary, no `.node` addon. See [`tauri-app/`](tauri-app/) and
> [`docs/TAURI_MIGRATION.md`](docs/TAURI_MIGRATION.md) for what's built and
> exactly how to build it. The old Electron/Node-API app (`electron/`, `napi/`)
> has been **retired** now that the Tauri build is verified working
> end-to-end — see `CHANGELOG.md` for the migration record.

> **The C++ CMake project is NOT the app.** It builds only the engine library
> (`netpulse_core`) and its unit tests (`netpulse_tests`). If you press VS Code's
> Debug button it will build and run `netpulse_tests.exe`, which prints
> `ALL TESTS PASSED` and exits with code 0. A console test finishing and closing
> its window in a flash is **success, not a crash** — that's the tests doing
> their job. The actual app lives in `tauri-app/` (below).


A cross-platform **desktop** path-latency monitor (PingPlotter / mtr style):
continuous per-hop ping + traceroute, IPv4 **and** IPv6, multiple targets, live
config, per-hop ASN/BGP, alerts, exports, light & dark themes.

Each target in the sidebar shows a **two-lamp status signal** (target + path) —
see [Status lamps](#status-lamps) below for what the colours mean.

Net Pulse — Open Net Tools is a **native desktop app** — a Tauri shell (Rust
host) + the same C++ engine, statically linked into the Rust binary via a
`cxx` bridge (no `.node`/shared-library addon shipped separately). There is
**no web server, no localhost port, and no WebSocket**. The renderer talks to
the engine only over Tauri's own `invoke` IPC, exactly like a Qt app talks to
its backend.

```
┌─────────────────────────────────────────────┐
│ Tauri webview (React UI, tauri-app/dist)      │  OS webview — no HTTP
│   invoke('add_target', …)  ──IPC──┐           │
├─────────────────────────────┼─────────────────┤
│ Rust host (tauri-app/src-tauri)  ▼            │
│   commands.rs → ffi.rs (cxx bridge)            │  in-process, statically linked
├───────────────────────────────────────────────┤
│ C++ engine (core/)  — ICMP codec, transport,   │  background threads
│ continuous probing, per-hop stats, JSON state  │  raw sockets
└───────────────────────────────────────────────┘
```

## Layout

| Path        | What                                                                 |
|-------------|----------------------------------------------------------------------|
| `core/`     | The engine (header-only API in `core/include/netpulse/`). Pure C++20. |
| `tauri-app/` | Desktop app: Rust host (`src-tauri/`, `cxx` bridge to `core/`) + React renderer (Vite). |
| `web/`      | Standalone React renderer (Vite), served over HTTP by `server/` — a separate deployment target, not used by `tauri-app/`. |
| `server/`   | Optional HTTP server (`server/main.cpp`) that serves `web/dist` for a browser-based deployment. |
| `tests/`    | C++ unit tests for the engine.                                        |
| `CMakeLists.txt` | Builds the core library + unit tests (for engine development).   |

## Prerequisites

- **Rust** (current stable, via [rustup](https://rustup.rs) — not your distro's
  package manager; see `tauri-app/src-tauri/Cargo.toml`'s `rust-version`).
- **Node.js 20.19+** and npm, for the React renderer.
- **CMake** 3.15+ on your **PATH** (for engine development / `ctest` — the
  Tauri build itself invokes the C++ compiler directly via `cxx_build`, not
  through this CMake project).
- A **C++20 compiler** (MSVC / GCC 10+ / Clang 12+).
- **Linux only:** `libwebkit2gtk-4.1-dev libgtk-3-dev librsvg2-dev libsoup-3.0-dev libjavascriptcoregtk-4.1-dev`.

## Build & run

```bash
cd tauri-app
npm install
npx tauri dev      # launches the app with hot-reload
# or:
npx tauri build    # produces a real installer (NSIS/AppImage/dmg per OS)
```

> **`npm install` needs one non-npm-registry download.** The Excel-export
> feature depends on `xlsx` (SheetJS), whose npm-published build is
> permanently frozen on an old, vulnerable release — see `tauri-app/.npmrc`'s
> comment for why. `package.json` installs it from SheetJS's own CDN
> (`cdn.sheetjs.com`) instead, and `.npmrc` permits that one exception to
> npm's default "no installing from a raw URL" policy. Two practical
> consequences: (1) `npm install`/`npm ci` needs outbound access to
> `cdn.sheetjs.com`, not just the npm registry — relevant if you're behind a
> restrictive corporate/CI firewall; (2) `.npmrc` must actually be committed
> to the repo for a fresh clone (including CI) to pick it up — it's not
> gitignored, but worth knowing if you ever regenerate project scaffolding.

See [`docs/TAURI_MIGRATION.md`](docs/TAURI_MIGRATION.md) for the full
architecture writeup, and [`docs/OBFUSCATED_BUILD.md`](docs/OBFUSCATED_BUILD.md)
for the optional `--features obfuscate` hardened build variant.

### Raw sockets / privileges

ICMP probing needs raw-socket privileges:

- **Windows:** run the app **as Administrator**.
- **Linux:** either run elevated, or grant the capability once:
  `sudo setcap cap_net_raw+ep $(which netpulse)` (the installed binary).
  Alternatively use **unprivileged datagram ICMP** by unchecking **Raw** when
  adding a target (works without admin on most Linux/macOS for plain ping).

## Engine development (no Node/Rust needed)

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build      # runs the C++ unit tests
```

## Security

- **No network surface from the app itself.** No HTTP server, no open port, no
  WebSocket — the only sockets opened are the ICMP probe sockets.
- **Narrow IPC surface:** every command the frontend can call is listed in
  exactly two places that must match — `invoke_handler` in `lib.rs` and
  `capabilities/default.json`'s `permissions` array. No filesystem, shell, or
  HTTP plugin is enabled; every network operation goes through this app's own
  named commands. See [`SECURITY.md`](SECURITY.md) for the full writeup.
- **Content-Security-Policy** (`tauri.conf.json`): `default-src 'self'`,
  `script-src 'self'` (no remote scripts, no `eval`), `object-src 'none'`,
  `frame-ancestors 'none'`.
- **Input validation:** every value crossing the Rust↔C++ boundary and every
  host string passed to a tool command is range-clamped/allowlist-validated;
  no shell is ever invoked (ping is spawned via argv, never a shell string), so
  there is no command-injection surface.

## Status lamps

Each target in the sidebar shows a **two-lamp signal**, read together like a
German railway *Hauptsignal* — the meaning comes from both lamps, not one
blended colour. The **left lamp is the target** (the destination's own health);
the **right lamp is the path** (the route/intermediate hops leading to it). This
separates "the destination is slow/lossy" from "something upstream on the route
is misbehaving", so you can tell at a glance *which* is the problem.

**Target lamp** (destination health), graduated by latency and loss:

| Colour | Meaning |
|--------|---------|
| 🟢 green | Healthy — low latency, no loss. |
| 🟩 lime *(pulsing)* | Healthy, but the route or latency baseline **changed recently** (e.g. the destination's IP moved to a different path / load-balancer, or the median RTT shifted) — shown until it re-stabilises (timer). |
| 🟡 yellow | Elevated — latency ≥ 70 ms **or** packet loss > 5%. |
| 🟠 orange | Degraded — latency ≥ 100 ms **or** packet loss > 10%. |
| 🔴 red | Unreachable, **or** latency ≥ 150 ms, **or** loss > 20%, **or** latency fluctuating with ≥ 50 ms swings. |
| 🔵 cyan *(pulsing)* | Route discovery in progress (no confirmed destination yet). |

**Path lamp** (route health):

| Colour | Meaning |
|--------|---------|
| 🟢 green | Packets routing cleanly to the target. |
| 🟡 yellow | An intermediate hop is dropping packets **or** not revealing itself (a `*`/silent router — often normal, as routers deprioritise ICMP to themselves). |
| 🟠 orange | Packets are routing toward the target pool (via BGP) but the target or some router is **dropping** them. |
| 🔴 red | No route — internet/interface down, RTO, or the first hop is rejecting. |
| 🔵 cyan *(pulsing)* | Route discovery in progress. |

The exact thresholds live in the `LAMP` object in `web/src/App.jsx`
(`destLamp` / `pathLamp`); the colours are CSS-variable-driven in
`web/src/styles.css` (`.lamp.st-*`). These thresholds are deliberately separate
from the single `alerts.ms` / `alerts.loss` pair, which drives the per-hop table
highlighting and the "N hops with loss/latency" banner.

> **Note for maintainers:** the lamp classes are composed at runtime
> (`'st-' + state`), so they never appear as literal strings for Tailwind's
> content scanner. They are listed in `safelist` in `web/tailwind.config.js` —
> **any new lamp state must be added there**, or Tailwind's tree-shaker will
> silently drop its colour from the build.

## Route discovery

Discovery (finding the hops to the destination) and steady-state monitoring use
different send cadences, governed by two cooperating mechanisms in
`core/src/session.cpp`:

1. **Per-hop desired cadence.** Unanswered hops become due quickly
   (`kDiscoveryInterval`) so the route is found fast; answered hops fall back to
   the configured steady `interval`. Non-responding (`*`) hops back off after a
   few fast tries (`kDiscoveryTries`) so they don't hog the budget.
2. **A single global token-bucket rate limiter** (`kMaxProbeRate` /
   `kProbeBurst`) paces **all** ICMP sends into a smooth, low, constant stream.
   This is deliberate: fast raw-socket ICMP bursts to one address are exactly
   the pattern that behavioural AV, Windows Defender's network monitor, and
   Smart App Control flag as flooding/scanning. The bucket caps the aggregate
   send rate at or below the app's own steady all-hops rate, so discovery is
   quick **without** ever emitting a flood/scan signature.

Two related correctness details handled here: a reply that comes back **later
than its timeout** is rejected (treated as loss) rather than recorded as a giant
RTT — this prevents the "latency ramp" artifact when an edge briefly
rate-limits/queues ICMP. And any **EchoReply** is accepted as reaching the
destination even if its source address was rewritten by a NAT/load-balancer, so
discovery isn't stalled by address rewriting (the visible destination IP is
updated to the actual responder).

## Styling

The UI is built with **Tailwind CSS**, compiled at build time in `web/` via
PostCSS (`npm run build`). This has **zero effect on packaging**: the output
is one static `.css` file in `dist`, exactly like the hand-written CSS it
replaced — Tailwind never ships in the app bundle, only its compiled
output does. Theme colors (light/dark) are still driven by CSS variables in
`src/styles.css`, with Tailwind utilities reading from them, so the existing
`data-theme` toggle keeps working unchanged. No web fonts are bundled or
fetched — the UI uses the OS's system font stack, which keeps licensing and
the CSP (`connect-src 'self' https:`) simple.

## Support the project

Net Pulse — Open Net Tools is free, open source, and independently developed.
If it's useful to you, consider
[sponsoring on GitHub](https://github.com/sponsors/ArchismanKarmakar) — it
directly funds continued development, a code-signing certificate (see
[`SECURITY.md`](SECURITY.md) for why that matters for clean AV/SmartScreen
results), and testing infrastructure.

## Architecture

For the engine's design, low-level threading model, and the reasoning behind
the socket pool / direct-echo measurement / shared-hop cache / loop auditor,
see [`ARCHITECTURE.md`](ARCHITECTURE.md).

## License

**GNU Affero General Public License v3.0 or later (AGPL-3.0-or-later)** — see
[`LICENSE`](LICENSE) and [`COPYRIGHT`](COPYRIGHT). Third-party components and
their licenses are listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

Net Pulse — Open Net Tools is copyleft, in the spirit of projects like VLC: it
is and stays open source. The AGPL additionally requires that if you run a
modified version as a **network service**, you must offer its users the
corresponding source. Contributions are welcome and expected to be under the
same license, keeping the project open for active community development.

"PingPlotter" and "mtr" are referenced only for behavioral comparison and are
trademarks of their respective owners; Net Pulse — Open Net Tools is an independent project.

## Tools (tabs)

The app is organized into tabs across the top:

- **Path / MTR** (home) — the live multi-target path-latency monitor.
- **Ping** — a single-host ping using the same native ICMP engine as the
  path monitor (pooled sockets, one shared reply-dispatch thread — not an OS
  `ping` subprocess), with live count/size/TTL/interval/family controls.
- **DNS Lookup** — forward (A/AAAA/CNAME) and reverse (PTR) resolution.
- **Port Scanner** — a bounded TCP connect scan (≤ 2048 ports/scan). Only scan
  hosts you own or are authorized to test.

The Ping, DNS, and Port Scanner tools run in the desktop app (they use the OS
network stack via the Rust host); they are unavailable in a plain browser dev
server.
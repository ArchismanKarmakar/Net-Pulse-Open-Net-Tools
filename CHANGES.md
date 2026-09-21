# NetPulse v1.1.2 — fixes and review

Drop these files into the corresponding paths in the repo (they're full-file
replacements, not patches). All C++ changes were verified with:

```
cmake -B build_test -DCMAKE_BUILD_TYPE=Debug
cmake --build build_test -j
./build_test/netpulse_tests   # ALL TESTS PASSED
```

## 1. Hostname + IP-type "conflict" — fixed (tauri-app/src/App.jsx)

Root cause, two separate bugs:

- `addTarget()`'s duplicate-check comment said identity is `(host, protocol,
  port)` and explicitly listed "IPv4 vs IPv6" as a legitimately different
  measurement — but the actual comparison had dropped `family` from the
  tuple, so adding the same host as v4 then v6 was rejected as "already in
  the list."
- `addTargetHost()` (used by the quick-trace menu items) hardcoded
  `family: 'auto'` on every call and deduped on hostname alone, so it could
  never add a second family for a host at all.

Fix: family is back in the identity tuple, compared via each target's
*resolved* family (`t.family`, "IPv4"/"IPv6") with a fallback to the
configured label while unresolved — so an "auto" entry that already
resolved to v4 correctly collides with a new explicit "v4" request, not just
with another literal "auto". `addTargetHost` takes a real `family` param.

Also added the behavior you described ("if one is present then add the
other"): if you add on Family=Auto and the host already has exactly one
family being traced, it now steers the request at the missing family
instead of silently re-adding a duplicate of the one that's already there.
If both already exist, it falls through to the duplicate modal as expected.

## 2. Force Recheck vs. Frankenstein routing — fixed (core/src/session.cpp, session.hpp)

Root cause: the engine's background "Frankenstein-route guard" (which wipes
a stale hop address once `kLegacyMissThreshold` legacy probes miss, spread
across `kLegacyMissWindowSecs` ≈ 45s) is deliberately conservative — it
needs the wall-clock spread to rule out ordinary jitter. But
`force_recheck()` only zeroed `hop_recheck_at`, which fires **exactly one**
legacy probe (it's reset to `now` the instant it's sent). One probe can
contribute at most one miss toward that 2-miss/45s-apart threshold, so a
single Force Recheck click could never decisively resolve a stuck hop — it
just nudged the same passive counter a background recheck would have
touched anyway.

Fix: Force Recheck now seeds a short **burst** of `kForceVerifyProbes` (3)
back-to-back legacy probes per already-addressed hop, sent at normal probe
cadence (completes in ~2-3s). If every probe in the burst misses, that's
treated as sufficient real-time evidence — several independent clustered
misses is at least as strong a signal as the wall-clock-separated pair the
passive guard requires — and the existing guarded wipe fires immediately.
Same safety gates apply either way (loss-based `kGuardedWipeMaxRecentLossPct`
check, `wipe_count` cap) — an explicit user request bypasses the *wait*, not
the safety check. If any burst probe gets a reply, the hop is confirmed and
the burst state clears.

New state: `force_verify_remaining` / `force_verify_misses` /
`force_verify_outstanding` maps, all cleaned up on reply, on burst
completion, and on a full route-context reset (added to that reset block
alongside `legacy_miss`/`hop_recheck_at`).

## 3. Private IPs in the shared-hop cache — fixed (core/src/transport.{hpp,cpp}, session.{hpp,cpp})

Root cause: `SharedHopTable` keys on `(source_addr, predecessor, ip)` and
excluded private/CGNAT `ip` outright, because a private IP is only
unambiguous *within one routing domain* — the same `192.168.1.1` could be
two different physical routers behind two different interfaces. The key
already includes `source_addr`, but that's almost always an empty string
(most targets don't explicitly bind an egress interface), so two sessions
on genuinely different interfaces would collapse onto the same key anyway —
exactly the ambiguity the restriction existed to prevent.

Fix — a "more elongated key" as you put it, but derived automatically
instead of requiring configuration: added `local_egress_ip(family, dest_ip)`
in `transport.cpp`, the standard "UDP `connect()` trick" — open a throwaway
UDP socket, `connect()` it to the real destination (this **never sends a
packet**; UDP `connect()` only consults the routing table), read back
`getsockname()`, close it. That's the OS's own routing table answering
"which interface would traffic to this destination actually leave from" —
the same decision the real probes get — so two sessions whose destinations
route out different interfaces (physical NIC vs. VPN adapter, etc.) get
different keys with zero configuration, while sessions that share an egress
(the common case) correctly share the cache.

`Session::resolve()` computes this into `local_egress_` whenever
`dest_`/`family_` are established. `effective_cache_source()` uses the
configured `source_addr` when set (unchanged, already unambiguous), else
falls back to `local_egress_`, else empty (falls back to the original
public-only behavior for that session — safe by construction, never wrong,
just conservative). The three `SharedHopTable` functions gate through a new
`cache_gate(ip, source)`: public IPs cacheable regardless of source (as
before); private/CGNAT cacheable only when `source` is non-empty; loopback/
link-local/unspecified/multicast never cacheable regardless (link-local in
particular can't be disambiguated by source at all — it's identical on
every interface by definition).

Updated `tests/test_core.cpp` to cover: private IP now shares correctly
across two publishes with the *same* non-empty source, stays independent
across two *different* non-empty sources for the same private IP, and still
never caches with an empty source (the original conservative fallback,
preserved for exactly the case it protects).

## 4. Backend review — other things worth fixing

Roughly in priority order:

- **`RdnsResolver::cache` (session.cpp) grows unbounded for the process's
  lifetime** — every distinct hop IP ever seen gets a permanent entry, with
  no eviction. Contrast with the frontend's own `hostCache`/`asn` caches,
  which already cap at `HOST_CACHE_MAX` with FIFO eviction. For a
  long-running deployment tracking 100+ targets over weeks, this is a slow,
  real memory leak. Fix: apply the same capped/FIFO (or LRU) pattern
  server-side.

- **`ColdStore` per-target files are never compacted.** `forget_target()`
  (already fixed to run on `remove()`) deletes a removed target's files, but
  a target that's added, removed, and re-added under the *same name* many
  times over a long-running install just keeps accumulating a fresh id and
  fresh files each time — nothing currently reclaims disk for a target
  that's still logically "the same trace" from the user's perspective.
  Worth deciding whether re-adding an identical (host, protocol, port,
  family) tuple should reuse the old id/history instead of always minting a
  new one.

- **CSV export duplication** (`append_target_rows`,
  `export_target_full_csv`, `export_all_targets_full_csv` in manager.hpp) —
  the per-hop `ColdStore::read_range` + hot-tier merge logic is correct but
  copy-pasted between the single-target and all-targets export paths only
  via the shared `append_target_rows` helper, which is good, but the
  CSV-header string itself is duplicated verbatim in both callers. Minor,
  but a future column addition is exactly the kind of edit that's easy to
  make in one and forget in the other.

- **The global pacer (`g_pacer()`) and `RdnsResolver`/RX-dispatcher
  singletons are deliberately leaked** (documented as intentional,
  process-lifetime). That's a reasonable choice for a long-running app, but
  worth double-checking under the `NETPULSE_OBFUSCATE` build variant and
  under ASan/LeakSanitizer runs in CI specifically with `LSAN_OPTIONS`
  suppressions for these — otherwise every CI run using ASan will flag them
  as leaks even though they're intentional, which either trains people to
  ignore real leak reports or forces a suppression file that needs to stay
  in sync.

- **IPv4 vs IPv6 vs protocol symmetry**: the fixes above (force-recheck
  burst, private-IP caching, family-aware dedup) are all written at the
  `Session`/`SharedHopTable` level and apply identically to ICMP, UDP, TCP,
  and HTTP — none of the four `run()`/`run_udp()`/`run_tcp()`/`run_http()`
  loops needed protocol-specific carve-outs, since `shared_publish`/
  `shared_adopt` and the legacy-miss/force-verify bookkeeping live in the
  one shared per-Session state block all four already read from. Confirmed
  by grep — all 6 `shared_publish`/`shared_adopt` call sites (one per
  protocol loop, plus the destination-shrink and direct-echo paths) now
  route through `effective_cache_source()` identically.

## 5. CLI tool (mtr-style) — scope, not yet built

This is a genuinely separate deliverable — a real cross-platform terminal UI
is a different engineering surface than the Tauri/React app, even though it
can reuse 100% of `core/`. Proposed shape, so we're aligned before I build
it:

- **New target**: `cli/` directory, `netpulse-cli` (or `npulse`) binary,
  linked against the existing `netpulse_core` static library — no changes
  needed to `core/` itself beyond what's already a clean public API
  (`Session`, `Manager`, `Settings`).
- **Rendering**: a small, dependency-light terminal renderer rather than
  pulling in ncurses (awkward on Windows) — raw ANSI escape codes for
  cursor positioning/color, with a `platform.hpp`-style abstraction for the
  one genuinely OS-specific bit (Windows Terminal needs
  `ENABLE_VIRTUAL_TERMINAL_PROCESSING` set via `SetConsoleMode`; POSIX
  terminals support ANSI natively). This mirrors how `platform.hpp` already
  isolates the one real OS difference in the socket code instead of
  branching everywhere.
- **Feature parity target for v1**: live per-hop table (loss/sent/recv/last/
  avg/best/worst/stdev — the same fields `mtr`'s report mode shows), `-4`/
  `-6`/auto family selection, `-i`/interval, `-c`/count-then-exit vs.
  continuous, protocol selection (icmp/udp/tcp/http — a real differentiator
  over `mtr`, which is ICMP/UDP only), and a `--report`/`--json` non-interactive
  output mode for scripting (this is where NetPulse-CLI can actually beat
  `mtr` outright, since `mtr --json` is one fixed schema and this can reuse
  the exact same state-JSON shape the desktop app already emits).
- **What it reuses vs. what's new**: reuses `Session`, direct-echo
  measurement, the shared-hop cache (including the private-IP fix above —
  a CLI instance run alongside other NetPulse processes on the same
  machine benefits automatically), and the loop-auditor/Frankenstein-guard.
  New code is entirely the terminal renderer and argument parsing —
  probably 800-1500 lines, no core engine changes required.
- **Build integration**: an `add_subdirectory(cli)` in the top-level
  `CMakeLists.txt`, gated behind a `NETPULSE_BUILD_CLI` option (default ON
  for a `cmake --build` from source, left out of the Tauri packaging
  pipeline which doesn't invoke plain CMake directly).

## 6. Verification performed (this round)

Beyond the compile + unit-test check noted at the top:

- Built and ran the full test suite under AddressSanitizer + Undefined
  Behavior Sanitizer — clean.
- Linked a small harness directly against the patched `netpulse_core` and
  ran **live** traces against a real IPv4 target (`1.1.1.1`) across all
  four protocols (ICMP/UDP/TCP/HTTP) — all four received real replies from
  the actual gateway hop with sane RTTs, clean under ASan too.
- Stress-tested Force Recheck with overlapping rapid calls mid-burst — no
  crash, no hang, no false wipe of a healthy hop, clean under ASan.
- IPv6 end-to-end live testing wasn't possible in this environment (no
  IPv6 stack in the sandbox at all — confirmed independently via a raw
  `AF_INET6` socket test, not a code issue). Verified the IPv6-specific
  logic directly instead: `is_public_ip`/`is_cacheable_ip`/`cache_gate`
  against real public, ULA, and link-local v6 addresses, and
  `local_egress_ip` failing gracefully to `std::nullopt` (not crashing or
  misbehaving) when IPv6 truly isn't available.
- Syntax-checked the `App.jsx` change with `esbuild` (no bundler/Tauri
  build available in this environment, but this confirms no syntax
  regression).

**A real, pre-existing bug this surfaced** (not introduced by the fixes
above, but found while confirming they apply identically across protocols):
`predecessor_of()` — the function that decides the shared-hop cache key —
was implemented two different ways in the same codebase. The ICMP loop used
a safe, deliberately-argued-for version; UDP, TCP, and HTTP all still used
an older, unsafe "walk back to the nearest resolved hop" version that the
ICMP code's own comment explicitly warns against (it can make two
genuinely different targets silently share one cache entry). Fixed — all
four protocols now use the identical, safe logic. See `CHANGELOG.md`'s
"Unreleased" section and `ARCHITECTURE.md` §4 for the full writeup.

## 7. Documentation updated

- `ARCHITECTURE.md` §4 — the private-IP cache-gate section rewritten to
  describe `cache_gate`/`local_egress_ip`/`effective_cache_source`, and the
  "finding the predecessor" subsection rewritten to match the actual
  (now-unified) code instead of a stale description that didn't match
  what shipped.
- `ARCHITECTURE.md` §6 — new subsections covering the silent-legacy-miss
  Frankenstein guard and the Force Recheck burst fix in full (this
  mechanism wasn't documented at the architecture level before, only in
  inline code comments).
- `CHANGELOG.md` — new "Unreleased" section with all four fixes above,
  matching the project's existing narrative changelog style.
- `cli/CLI.md` — full command/help reference for the planned CLI (see §5
  above) — see next section.

## 8. CLI command/help reference

Written up as `cli/CLI.md` in this archive: full `--help`-equivalent
reference for the planned `netpulse-cli`/`npulse` binary — synopsis, every
flag (family, protocol, timing, output mode), the interactive live-view
keyboard shortcuts (including a `Force Recheck` key mapped to the exact
same burst mechanism as the desktop app's button), exit codes, permissions
per OS, and a feature comparison against `mtr`. This documents the intended
interface **before** writing the implementation, same as the scope in §5 —
the CLI itself still isn't built. Let me know if you'd rather have the tool
built first and the docs written to match afterward instead.

## 9. Real IPv6 bug found and fixed (this round)

You asked me to specifically check the IPv6 issues rather than accept last
round's "sandbox has no IPv6" explanation at face value. Went through every
IPv6-specific code path in `core/` line by line looking for something that
would break on a real IPv6-enabled machine even though this sandbox
couldn't surface it live. Found one real bug:

**`Family: IPv6` couldn't tell "no real IPv6" apart from "link-local IPv6
only."** `Session::run()`'s pre-flight check for "does this machine have a
usable local egress for this family" (`has_local_v4`/`has_local_v6`)
counted *any* address of the right family on *any* active interface —
including link-local (`fe80::/10`, and IPv4's `169.254.0.0/16` APIPA
equivalent). Link-local addresses are auto-assigned to essentially every
active interface by SLAAC/APIPA on every major OS **regardless of whether
the machine has any real route to the internet at all** — so this check
passed almost universally, IPv6 connectivity or not. With `Family: IPv6`
explicitly selected on a machine that only has link-local v6 (i.e. no real
v6 in practice — a very common configuration on IPv4-only home/office
networks, since the OS still auto-configures link-local regardless), the
engine skipped straight past its own "No local IPv6 egress available —
waiting for IPv6 or change family" message and opened a real socket
against a destination it could never reach, producing confusing
0%-progress behavior instead of a clear explanation.

Fixed by filtering interface addresses through `is_cacheable_ip()` (the
same function §3's private-IP cache fix uses) before counting them —
it already excludes link-local/loopback/unspecified while correctly still
accepting private/CGNAT addresses, which *are* a real, valid egress (a
home LAN behind NAT reaches the internet fine). Applied symmetrically to
both the IPv4 and IPv6 sides of the check. Added direct unit-test coverage
for `is_cacheable_ip()` itself, which previously only had indirect coverage
via the shared-hop-cache tests.

**Everything else IPv6-specific was audited and confirmed correct**, not
just assumed: ICMPv6 type codes (128/129/3/1 — correct per RFC 4443, and
correctly distinct from ICMPv4's 8/0/11/3), the deliberate omission of
ICMPv6 checksum computation in `build_echo()` (correct — RFC 3542-compliant
raw ICMPv6 sockets have the kernel compute and overwrite it regardless of
userspace, already documented in `ARCHITECTURE.md` §5), the
no-IP-header-prepended raw-socket convention `parse_v6()` correctly
assumes, and `IP_TTL`/`IPV6_UNICAST_HOPS` branching across all three
protocol implementations (HTTP inherits it via `ProbeTcp`). None of these
needed changes — they were already right.

**Re-verified after this fix**: full rebuild + unit tests (plain and
ASan+UBSan) clean, and re-ran the live all-protocol/all-family harness from
last round — IPv4 still gets real replies across all four protocols, and
the genuinely-IPv6-less sandbox still correctly shows the "No local IPv6
egress" message rather than silently misbehaving (confirming the fix
tightened the check without breaking the legitimate no-IPv6 case it was
already handling correctly).

See `CHANGELOG.md`'s "Unreleased" section and `ARCHITECTURE.md`'s new
"Family availability" subsection (end of §9) for the full writeup.

## 10. Critical no-IPv4/no-IPv6 alert, Interfaces diagnostics page, CLI auto-completion

Three new pieces added on top of everything above:

### Critical connectivity alert (App.jsx)

A new periodic check (on mount, then every 20s) compares against the exact
`usable` classification the probing engine itself uses (`is_cacheable_ip()`
— see §9's IPv6 fix), and fires a **real OS-level critical alert sound**
plus a blocking modal when either family has no usable egress anywhere on
the machine:

- No usable IPv4 anywhere → "No IPv4 connectivity detected"
- No usable IPv6 anywhere → "No IPv6 connectivity detected"
- Neither → one combined "No network connectivity detected" alert (never
  two blocking modals back to back)

I did **not** build a new sound mechanism — the app already has a real,
native `play_alert_sound(kind)` FFI function (`MB_ICONHAND` "Critical Stop"
sound on Windows, the user's own chosen alert sound via
`kSystemSoundID_UserPreferredAlert` on macOS, documented no-op on Linux —
there's no cross-desktop-environment standard alert API to hook there) with
an established `playAlertSound('error')` + `showModal({blocking: true})`
pairing already used elsewhere in the app (the "administrator elevation
required" dialog). This reuses that exact pattern rather than inventing a
synthesized Web Audio beep, which would have been a second, different,
non-native sound competing with the app's existing one.

Each condition is independently "armed": it alerts once when the problem
appears, stays quiet on every subsequent poll while the problem persists
(a real, ongoing outage shouldn't nag every 20 seconds), and re-arms the
moment the condition clears — so it will alert again on a genuinely new
recurrence (unplug/replug, VPN drop, etc.) rather than only ever once per
app launch.

### Network Interfaces diagnostics page (new tab)

New "🖧 Interfaces" tab (`InterfacesPage.jsx`) listing every adapter on the
machine — up or down, including loopback — with address, family, status,
MTU, and the same `usable` flag the critical alert and the engine's own
egress check use. Backed by a **new, separate** `/api/interfaces/detailed`
endpoint (`list_interfaces_detailed_json()`/`list_interfaces_detailed`
Tauri command) rather than changing the existing `/api/interfaces` the
source-address dropdown already relies on — that one's filtered/3-field
shape is unchanged, so nothing about the existing dropdown's behavior
shifts.

Backend support for this: `NetInterface` (`transport.hpp`) gained `up`/
`loopback`/`mtu` fields (backward compatible — default member initializers
mean every existing 3-field aggregate-init call site still compiles), and
`list_interfaces()` gained an `include_all` parameter (default `false` =
byte-for-byte the same filtered behavior as before) that, when `true`,
returns every adapter with real MTU (via `SIOCGIFMTU` on POSIX, the `Mtu`
field on Windows) and accurate up/loopback flags.

**Verified**: rebuilt and re-ran the full test suite (plain + ASan/UBSan)
clean, and live-ran a small standalone harness against the patched
`list_interfaces()` on this sandbox — default mode still returns exactly
what it did before (just the active NIC), `include_all` correctly adds the
loopback adapter with its real MTU (65536) and populates the active NIC's
real MTU (1400) via the new `SIOCGIFMTU` path, proving it's not just
compiling but actually working. The Rust/cxx FFI wiring
(`ffi.rs`/`commands.rs`/`lib.rs`/`netpulse_ffi.hpp/cpp`) follows the
existing `list_interfaces`/`list_interfaces_json` pattern exactly but
**could not be build-verified** in this environment (no Rust/cxx toolchain
available) — flagging this honestly rather than claiming a check I didn't
actually run. The JavaScript side (`App.jsx`, `InterfacesPage.jsx`,
`lib/api.js`, `tauri-bridge.js`) was syntax-checked with `esbuild`,
including a bundled local-import-resolution pass confirming the new
component's imports and the new endpoint wiring all resolve correctly.

### CLI auto-completion (spec addition, `cli/CLI.md`)

Added a full "Shell auto-completion" section to the CLI spec: a
`npulse completion SHELL` subcommand (bash/zsh/fish/PowerShell) generating
a native completion script per shell — the same pattern `kubectl`/`docker`/
`gh` use — rather than a bespoke in-process suggestion engine, so it
inherits each shell's own fuzzy-filtering, arrow-key selection, and
compatibility with prompt frameworks (starship, oh-my-zsh, PSReadLine) for
free. Completes flags, enum flag values (protocol/family), and HOST itself
from a small shared recent-targets file the desktop app already
maintains — a host traced in the GUI completes in the CLI and vice versa.
Since the CLI binary itself still isn't built (see §5/§8), this is a
specification addition only, same caveat as before.

## 11. Standard commands, build integration, and installer bundling (this round)

Delivered exactly what was asked, in three parts:

### `npulse` grew real standard-command support

`cli/main.cpp` now implements `ping`, `trace`/`traceroute`/`tracert`/`mtr`,
`ifconfig`/`ipconfig`/`interfaces`, `dns`/`nslookup`, `portscan`, and
`completion` — all genuinely built and tested, not just specified. Every
subcommand reuses an existing, already-tested engine call (`PingRun`,
`Session`, `list_interfaces`) rather than adding new probing logic.
`argv[0]`-based dispatch means a copy or symlink of the binary named
`ping`/`mtr`/`tracert`/`ifconfig` behaves like that command with no
subcommand needed — confirmed by literally copying the built binary under
each name and running it.

**Verification performed**: built clean with cmake on first try; ran every
subcommand against real traffic (a real reachable gateway on this
sandbox, since `1.1.1.1` itself isn't reachable from here — confirmed
that's an environment limit, not a CLI bug, by testing against the
gateway directly); found and fixed one real bug during this testing
(`trace -c 1` capturing an almost-empty first snapshot); reran everything
clean under ASan+UBSan afterward, plus the existing `netpulse_tests` suite
still passes unmodified.

### Repo integration: separate executable, one build, bundled into every installer

- `CMakeLists.txt`: `NETPULSE_BUILD_CLI` option (default ON) plus
  `add_subdirectory(cli)` — a plain `cmake --build .` from a source
  checkout now produces the CLI alongside the existing engine tests, no
  extra flags needed.
- `tauri.conf.json`: `bundle.externalBin` (Tauri's documented sidecar
  convention — cross-checked against Tauri's current official docs rather
  than assumed) plus `bundle.linux.deb.files` to additionally place a copy
  at `/usr/bin/npulse` for automatic PATH availability on Debian-based
  installs.
- `windows/hooks.nsh`: extended with CLI PATH registration (NSIS's `EnVar`
  plugin, HKCU-scoped) and a pre-install `taskkill` addressing a real,
  documented Tauri/NSIS gotcha (a sidecar binary can fail to get replaced
  on a same-version reinstall) found via a targeted search rather than
  assumed.
- All three relevant workflows (`tauri-ci.yml`, `tauri-release.yml`,
  `tauri-canary-build.yml`) now build the CLI sidecar for each job's exact
  OS/architecture before the Tauri packaging step, using the identical
  command sequence I verified works end-to-end on this machine first.

**Honest scope boundary, stated plainly rather than glossed over**: the CLI
binary itself — compiling, running, correctness — is genuinely verified on
this machine. The actual `tauri build`/GitHub-Actions/NSIS-install pipeline
that bundles it into a real Windows/macOS/Linux installer could **not** be
run here (no Tauri/cargo toolchain, no real Windows or macOS machine
available in this environment) — that part is written as carefully as
possible against Tauri's and NSIS/EnVar's documented, current behavior
(verified via targeted lookups this round, not assumed from memory alone),
but it has not been exercised by an actual install the way everything else
in this round has been. macOS PATH registration specifically isn't solved
yet at all (a `.dmg` has no script-execution point) — documented as a known
gap with a concrete proposed fix (an in-app "install to PATH" action,
VS Code's `code`-command pattern) rather than a half-verified attempt.

### `CLI.md` rewritten to match what was actually built

The command reference now documents the real, implemented subcommand
interface (`npulse ping|trace|ifconfig|dns|portscan|completion`) rather
than the earlier flat `npulse [OPTIONS] HOST` sketch from before
implementation started — subcommands turned out to map more naturally onto
the genuinely different engine calls each one needs than one shared flag
set would have.

## 12. Answering: does the CLI run standalone, is packaging correct, dev commands

**Yes, the CLI runs completely standalone** — verified, not just claimed:
`ldd` on the built binary shows only `libc`/`libstdc++`/`libm`/`libgcc_s`,
nothing GTK/WebKit/X11/Wayland — zero GUI dependency. It's a plain console
app; this entire round's CLI development and every test I ran was done in a
headless Linux container with no GUI stack at all, which is itself a live
demonstration of exactly the "Arch with no desktop" / "Windows as a
separate process" scenario asked about, not just a theoretical claim.

**Packaging: correct as far as I can verify, with one real gap found and
fixed, and one real gap still open.** The gap I found: this project has no
native Arch package (no PKGBUILD/AUR), and while the Linux AppImage does
contain the CLI internally, an AppImage has no install step at all, so
there was no way to extract just the CLI from it easily — meaning Arch
(and any non-Debian Linux) had no practical way to get the CLI alone.
Fixed: the release workflow now ALSO publishes the CLI as its own
standalone per-OS download on the GitHub Release (`npulse-<os>[.exe]`),
independent of every installer. The gap still open: macOS PATH
registration (unchanged from last round — a `.dmg` has no script-execution
point; needs an in-app action instead). Same honest boundary as before on
the Windows/Linux installer-bundling side: written to match Tauri/NSIS's
documented conventions, YAML-validated, but not run through an actual
`tauri build`+install cycle (no Tauri toolchain or real Windows/macOS
machine available here).

**Dev commands** — added to `README.md`:
```sh
# GUI, full (Tauri window + hot reload):
cd tauri-app && npm install && npx tauri dev
# GUI frontend only (Vite dev server, no Tauri window/native calls):
cd tauri-app && npm install && npm run dev
# CLI only (no Node/Rust/Tauri needed at all):
cmake -S . -B build && cmake --build build --target npulse
./build/cli/npulse ping 1.1.1.1        # Linux/macOS
# build\cli\Debug\npulse.exe            # Windows (Visual Studio generator default)
```

## 13. Real bug from your live Windows test: `tauri dev` crashed on a fresh checkout

Your `npx tauri dev` output showed the exact failure mode: `bundle.externalBin`
made `build.rs`'s `tauri_build::try_build()` hard-fail immediately —
`resource path binaries\npulse-x86_64-pc-windows-msvc.exe doesn't exist` —
because nothing had built that file yet. I'd only wired the sidecar build
into CI, not into a plain local dev run, so this broke `tauri dev` for
anyone building fresh, which is worse than any of the packaging gaps
flagged so far since it blocks development entirely, not just release
polish.

**Fixed in `build.rs`**: it now builds the CLI sidecar itself automatically
(same `cmake` invocation the rest of the file already uses for the C++
engine) before calling `tauri_build::try_build()`, and skips that entirely
on every subsequent build once the file exists — so this costs nothing
after the first `cargo build`/`tauri dev`. If `cmake` genuinely isn't
available, it now fails with the exact commands to run manually instead of
the cryptic message you hit.

**Verification, honestly scoped**: I have no Tauri/cargo toolchain in this
environment to compile the real, full `build.rs` (it depends on `tauri`,
`tauri-build`, `cxx`, `cxx-build` from crates.io, and the actual GUI link
step needs system WebKit/GTK this sandbox doesn't have either). What I
could and did do: extracted the new function verbatim and ran it as a
standalone Rust program against the real repository — a fresh build (full
cmake output, sidecar produced), a second run confirming the skip-if-exists
path (no rebuild, instant), and a forced-failure run confirming the panic
message is the clear, actionable one, not a crash. Then syntax/type-checked
the complete `build.rs` directly with `rustc` — the only two errors are the
external crates this sandbox can't fetch (`tauri_build`, `cxx_build`);
everything else, including every line I touched, parses and type-checks
clean. That's a real step up from "written to match documented conventions"
for the parts of your report I could actually reproduce and fix here — but
the one thing I still can't do is run your exact `npx tauri dev` myself, so
please let me know if this doesn't fully resolve it on your machine.

## 14. CLI matured: tracert/tracert-mtr split, GUI-parity options, auto-refresh

Addressed directly:

- **`trace` → `tracert` + `tracert-mtr`**: `tracert` is now a real one-shot,
  progressive traceroute (Windows `tracert`/Linux `traceroute` style —
  prints each hop once as it settles, stops at the destination); the old
  continuously-live table moved to its own `tracert-mtr` (alias `mtr`)
  command, matching the GUI's Path/MTR view specifically.
- **More GUI-parity options**: `-T`/`--trace-interval` (route re-discovery
  interval, the GUI's "Trace" field), `-I`/`--interface` (source address,
  the GUI's "iface" dropdown — added to `ping` too), `-W`/`--timeout`,
  `-s`/`--payload`, `--unprivileged` (raw/privileged toggle).
- **Auto-refresh instead of Force Recheck**: exactly as scoped — `Family:
  Auto` in `tracert-mtr` now detects a persistently stuck "no local egress"
  state and automatically tears down and rebuilds the session every few
  seconds until it resolves, since the underlying `Session::resolve()`
  only ever runs once at start and has no other way to notice a route
  coming up later. A pinned `-4`/`-6` deliberately never does this. Also
  added `ifconfig -w`/`--watch` as the adapter-list equivalent.

**Verification, done for real**: full rebuild, `netpulse_tests` clean
(plain + ASan/UBSan). Ran every command against real traffic: `tracert`
and `tracert-mtr` both against a real reachable gateway, `ping -I`,
`tracert -T -s -I` together, `ifconfig -w` (confirmed it redraws on a
timer and exits cleanly on signal). For the auto-refresh mechanism
specifically — the most structurally new piece (a background poller
thread combining a real SIGINT with an internal restart decision into the
one stop flag `Session::run()` accepts) — ran it three ways: `Auto` family
against a target this sandbox genuinely can't reach (confirmed it retries
every ~3s, redrawing each time), the same target with `-6` pinned
explicitly (confirmed zero retries — correctly NOT self-healing a
deliberate family choice), and a live Ctrl-C sent mid-retry-loop
(confirmed immediate, clean shutdown). All three clean under ASan/UBSan,
including the new thread.

## 15. Real bug from your live Windows run: `tracert-mtr` not updating in place, plus a professional visual pass

Your output showed the exact symptom: a fresh table printed below the last
one on every refresh, on plain Windows PowerShell. Root cause and fix,
found and applied for real:

- **`term_init()` existed but was never called.** The file already had a
  well-built fix in progress (Windows VT-mode enabling via
  `SetConsoleMode`, flicker-free in-place redraw helpers, color) — but
  nothing in `main()` actually invoked `term_init()`, so `g_use_ansi` was
  permanently false and none of it ever activated. Fixed: called once at
  the top of `main()`, before any command runs.
- **`ifconfig -w` was still on the old, broken path** — a raw, unconditional
  `\033[2J\033[H` full clear every cycle, the same class of bug as the
  `tracert-mtr` report, plus its own separate flicker problem even where
  VT mode does work. Rewritten to use the same in-place redraw/color
  machinery as `tracert-mtr`.
- **Report mode was leaking redraw codes when run on a real terminal** — a
  gap I found while re-verifying, not something you reported: `--report`
  is supposed to never include control codes at all (for piping/
  scripting), but the per-line/end-of-frame clear codes were gated only on
  "is this a real terminal," not "is this actually a live redraw" — so
  running `--report` directly in a terminal (rather than piped) would
  still emit a few stray codes. Fixed with a `g_frame_live` flag scoped to
  actual redraw calls only; color is untouched (correctly still applies to
  `--report` on a real terminal, exactly like `git status`).
- **Presentation redesigned to look professional**: bold headers, a clean
  separator rule, green/yellow/red loss-percentage coloring matching the
  GUI's own thresholds, the GUI's ★ destination marker, long hostnames
  truncated with `…` instead of breaking alignment — applied to both
  `tracert-mtr` and `ifconfig -w` consistently.

**Verification**: since this environment has no real interactive Windows
session, I used Linux's `script` command to allocate a genuine
pseudo-terminal and captured the raw bytes `npulse` actually emits — not
just eyeballing output, reading the literal escape sequence back. Confirmed
live mode emits exactly `\033[H` once per frame, `\033[K` after each line,
`\033[J` once at the end (the correct flicker-free sequence); confirmed
`--report` on that same real terminal now emits zero redraw codes while
still showing color; confirmed piped output (no pty) suppresses all color
and control codes automatically, as before. Full rebuild, `netpulse_tests`,
and every live command re-run clean under ASan/UBSan throughout.

The one thing I still can't do from here is confirm this looks right in
your actual Windows PowerShell/Windows Terminal — the underlying mechanism
(`SetConsoleMode` + `ENABLE_VIRTUAL_TERMINAL_PROCESSING`) is Microsoft's own
documented, standard way to enable this, and the escape-sequence behavior
is now verified correct at the byte level on a real terminal here, but
please let me know if it doesn't render as expected on your machine.

## 16. Real bugs from your second Windows run: mojibake, confusing "*" rows, and maturing ping/tracert

Two concrete, root-caused fixes plus a presentation pass across the two
commands you called out as immature:

- **"Γÿà" was mojibake, not a rendering fluke.** The destination marker
  (★), the truncation ellipsis (…), and the loop-warning icon (⚠) were all
  UTF-8 multi-byte sequences. A Windows console not explicitly told it's in
  UTF-8 mode reads each BYTE of a multi-byte character as its own separate
  legacy-codepage character — that's exactly what garbled "★" into "Γÿà".
  Fixed by removing every non-ASCII byte from anything actually printed
  (replaced with `[DEST]`, `...`, `!`) rather than trying to get every
  possible Windows codepage/font combination to cooperate — plain ASCII is
  guaranteed correct everywhere, no configuration required. Also added
  `SetConsoleOutputCP(CP_UTF8)` as a backstop for genuinely unavoidable
  non-ASCII content (an international hostname, say), and caught the exact
  same latent bug in the help text's em-dashes before it could be reported
  too. Confirmed zero non-ASCII bytes remain in any command's output by
  grepping the actual binary's output, not just reading the source.
- **Hops showing `*` with full, real, healthy stats right after a route
  change — that's real engine behavior surfacing (the guarded-wipe/
  Frankenstein-route guard, per ARCHITECTURE.md §6, only wipes a hop that
  WAS looking healthy, so its stats window still has good recent samples
  right after being wiped), but the rendering made it look like a bug.**
  Now distinguishes "never resolved" (`*`) from "just wiped, real data
  still pending re-verification" (`(re-resolving)`) — same accurate
  underlying data, self-explanatory instead of looking broken.
- **`ping` and `tracert` matured to the same presentation level as
  `tracert-mtr`**, not left plain: color-coded replies and RTT severity
  (green/yellow/red, matching the GUI's own thresholds), bold banners and
  summaries, protocol/payload/probe-count in `ping`'s banner, a
  `[DEST]`-tagged colored final row in `tracert`.

Verified: full rebuild, `netpulse_tests` clean (plain + ASan/UBSan) with
every command re-run under a real pseudo-terminal (`script`) and checked
byte-for-byte for stray non-ASCII output — not just visually inspected.

## 17. Clarified feature: `npulse.exe` opens a real shell, VS Dev Command Prompt style

Your clarification changed the actual ask: not a custom REPL, but exactly
what VS Developer Command Prompt does — `cmd.exe` itself, with an
environment tweak (PATH) applied first, opened by double-clicking the exe
or a shortcut to it, staying open afterward instead of flashing shut.
Implemented:

- `has_fresh_console()` — `GetConsoleProcessList()` returning exactly 1,
  the standard Microsoft-documented technique for "this console will close
  when I exit" (double-click/shortcut) vs. "I'm in an already-open shell"
  (typed at cmd/PowerShell/Windows Terminal) — confirmed against Microsoft's
  own official guidance on this exact question before relying on it.
- `launch_shell_console()` — prepends `npulse`'s own folder to `PATH` for
  the session, prints a short banner, then spawns `%COMSPEC%` (`cmd.exe`)
  inside the same console and waits for it, so the window stays open and
  behaves as an ordinary shell the whole time.
- Only triggers when BOTH no arguments were given AND the console is
  fresh — `npulse` typed with no arguments into an already-open shell is
  completely unaffected, still prints help exactly as before.

**A real verification upgrade this round**: installed a MinGW-w64
cross-compiler and Wine specifically so Windows-specific code isn't just
manually reviewed anymore. Cross-compiled the ENTIRE project (engine + CLI)
for a real `x86_64-pc-windows` target and ran the actual `.exe` under Wine
— confirmed real `ping` replies, real adapter enumeration via
`GetAdaptersAddresses` (not a stub), and real DNS resolution, all producing
correct output against genuine network traffic. This is meaningfully
stronger evidence than anything I could offer for the Windows-specific
work in earlier rounds of this conversation, and I'd use this same
approach retroactively on request if you'd like more confidence in any of
the earlier unverified Windows pieces (the NSIS/EnVar PATH registration,
in particular, is a good candidate — Wine does support NSIS installers to
a reasonable degree).

**Honest limit**: Wine's console subsystem doesn't perfectly replicate the
low-level process-attachment bookkeeping real Windows uses for
`GetConsoleProcessList()`, so I could confirm the surrounding logic (args
present → correctly skipped; the banner and PATH-prepending logic;
real command execution) but not the exact fresh-vs-inherited boundary
itself end-to-end. That specific behavior is implemented using the
correct, standard, documented technique, but needs a real Windows machine
for a final confirmation — please try double-clicking the built `.exe`
(or a shortcut to it) and let me know what you see.

## 18. Corrected scope: the console feature triggers on every OS and invocation style

Your clarification made clear the first version answered too narrow a
question — gated only on a freshly-allocated Windows console (the
double-click case), leaving "typed bare into an already-open terminal"
still just showing help, and doing nothing at all on Linux/macOS. Fixed:
`launch_shell_console()` now runs uniformly whenever `npulse`/`netpulse`
is invoked with zero arguments, regardless of OS or whether the terminal
session is fresh or already open — Windows spawns `%COMSPEC%`, POSIX
spawns `$SHELL` (falling back to `/bin/sh`) via `fork()`/`execl()`. An
alias invocation (`ping`, `tracert`, `mtr`, `ifconfig`) with no arguments
still shows that alias's own usage, unaffected — you asked for that
specific tool, not npulse's general console.

**This round I could verify completely, not just cross-compile**: since
Linux is the actual environment this was built in, I ran the real thing —
confirmed a bare `npulse` spawns the real shell, confirmed `PATH` inside
that spawned shell genuinely has npulse's directory prepended (`which
npulse` resolves it), confirmed `npulse help` works from inside that
session, confirmed the spawned shell's exit code propagates back
correctly, and confirmed every alias with no arguments keeps its own
distinct behavior rather than entering console mode — tested with both
`/bin/sh` and `/bin/bash` explicitly.

One thing worth being transparent about from this process: while
re-verifying the Windows side under Wine, a run of the exact same feature
appeared to hang. Rather than assume it was a real bug (or dismiss it as
Wine being Wine), I isolated the exact `CreateProcessA` pattern into a
minimal standalone test, confirmed THAT worked correctly on its own, then
retried the actual `npulse.exe` and got clean, stable results across
multiple repeated runs — concluding it was a one-off Wine flake, not a
defect in the code. Flagging this because "I tested it and it seemed to
hang once" is exactly the kind of ambiguous signal worth resolving with a
targeted follow-up test rather than either panicking or hand-waving past
it, and I'd rather show that process than pretend every test passed
cleanly on the first try.

## 19. Real bug from your Windows screenshot + report: aliases weren't backed by real files, and [DEST] could vanish

Two genuine, root-caused fixes:

**`tracert-mtr` (and `ping`/`tracert`/`mtr`/`ifconfig`) not recognized
inside the console.** My own banner promised these work directly, but I'd
only ever prepended `npulse`'s own directory to PATH — which makes
`npulse` runnable by name, but does nothing for any of the other names,
since `argv[0]` dispatch only activates for a file that actually exists
under that name somewhere on PATH. Nothing ever created one. Fixed for
real: the console now creates a temporary, session-scoped directory of
hard links to the executable under each standard name, prepended to PATH,
cleaned up automatically on exit. Also added `tracert-mtr` itself as a
recognized alias name (only `mtr` was recognized before).

**The mojibake in your screenshot** — I checked, and the specific
`truncate()` ellipsis bug is already fixed in current source (matches a
previous round's fix), so that screenshot most likely reflects a build
from before that fix. But re-sweeping the *entire* file for real this
time (not just the specific characters I already knew about) found two
genuine remaining em-dashes I'd missed, in the Auto-family retry message
and the `ifconfig -w` refresh message — fixed those too, proactively,
before they could be separately reported.

**The `[DEST]` marker could be silently truncated away** on a long
hostname — a real, separate bug your screenshot's garbling made me look
harder at the surrounding code, where I found it. Fixed by truncating the
hostname first and appending `[DEST]` after, so it's now always visible.

**Verification, and an honest unresolved finding**: fully verified on
Linux, including the exact reported scenario (`tracert-mtr` typed inside
a live spawned console session), clean under ASan/UBSan. On the
cross-compiled Windows `.exe` under Wine, I verified every individual
piece works (hard-link creation in isolation, a renamed alias binary run
directly) — but the *full* nested chain (console spawns a shell, which
spawns the alias binary) hung reliably under Wine specifically, which I
could not resolve or explain with certainty. I'm being upfront that this
is unresolved rather than claiming it's fixed: it's most consistent with
a Wine limitation given every individual piece checks out, but I have no
way to confirm that without a real Windows machine. If `ping`/`tracert`/
etc. hang or misbehave when typed inside the console on your actual
machine, that's the first place to look — and `npulse tracert-mtr HOST`
(the subcommand form, no nested alias involved) is a safe fallback that
doesn't touch this code path at all.

## 20. Real bug from your live PowerShell report: prompt corruption after `tracert-mtr`, plus a visible "you're in npulse" indicator

Root-caused and fixed properly:

- **Terminal corruption, PowerShell-specific**: a live-redraw view
  repositioning the cursor in the same buffer PSReadLine tracks desyncs
  PSReadLine's own bookkeeping from the real cursor position — the exact
  mechanism behind the garbled prompt fragments you saw. Fixed with the
  standard technique: `tracert-mtr`/`ifconfig -w` now switch to the
  alternate screen buffer for the whole session (same guarantee
  `vim`/`htop` give on quit), rather than repositioning the cursor in the
  shell's own buffer. `cmd.exe` (what the `npulse` console hosts) has no
  line-editing layer to desync, which is exactly why you saw it only from
  direct PowerShell usage.
- **Two further defensive layers**: Windows console mode is now saved and
  restored on exit (a child process leaving shared console state changed
  is a separate way to confuse a parent shell), and stray unread input is
  flushed on exit (some terminal interactions can leave response bytes
  queued that would otherwise resurface as garbage in the next program to
  read input).
- **The visible indicator you asked for**: a window/tab title, plus —
  inside the `npulse` console specifically — the shell's own prompt
  prefixed with `[npulse] `. I found the title-setting function had been
  written but never actually called anywhere (same "defined but dead"
  pattern as `term_init()` earlier), fixed that.

**Verification**: captured the actual raw escape bytes under a real
pseudo-terminal on Linux — not just visual inspection — confirming the
alt-screen sequence and title are correct, confirmed the `[npulse]`-
prefixed prompt renders live, confirmed a real SIGTERM mid-session
completes in ~3 seconds (not a hang). Re-verified on the cross-compiled
Windows `.exe` under Wine across multiple runs.

Two apparent hangs during this testing were investigated and resolved
rather than either dismissed or left unexplained: one traced specifically
to the `script` pty-recording tool's own known quirks (confirmed by
removing it and watching the identical command complete cleanly on its
own), the other a single non-reproducible Wine flake (three immediate
reruns were clean). Both are documented plainly in `CLI.md`/`ARCHITECTURE.md`
as testing-environment artifacts, not defects in this code.

## 21. Your two live-usage reports: a real cache-attribution bug fixed, and a timing question answered (not a bug)

**1. "stale just now showed live via another target, why this will show? we have a global cache right and also at that moment there was only 1 target" — real bug, fixed.**

Read the current `SharedHopTable`/`shared_last_seen_from` code (`core/src/session.cpp`,
`core/src/stats.cpp`) end to end. The "seen elsewhere" index
(`ip_last_seen`) stored only a timestamp — no record of *who* published it.
A hop's own last real reply is recorded as `stale_since` at the exact
moment it goes stale, and that same reply is always inside the 30s window
this check uses, by construction. So the check was really answering "has
this IP been heard from recently by anyone, including the hop's own very
recent past", not "by someone else right now" — meaning it would routinely
say "live via another target" using nothing but the hop's own
immediately-prior reply, exactly matching what you saw with only one
target running.

Fixed properly, not patched around: `ip_last_seen` now records the
publishing session's id alongside the timestamp, and the read excludes the
caller's own id — the same owner-exclusion the RTT-adoption side of this
cache (`shared_adopt_from`) already had. Added three new test cases
(`tests/test_core.cpp`) specifically for this: self-publish-then-self-query
now correctly sees nothing; a genuinely different session's publish is
still correctly surfaced (the real ECMP/asymmetric-routing case this
feature exists for); `max_age` expiry still applies regardless of owner.
Full `netpulse_tests` suite rebuilt and re-run — all passing, including the
new cases.

**2. "force refresh checked stale target and resumed it, but auto refresh did not, can you check if auto refresh has it or I got it wrong" — you didn't have it wrong, and there's no missing feature; it's a real, large timing gap.**

Read the current guarded-wipe/Force Recheck code (`core/src/session.cpp`)
in full. There is no separate "auto refresh" feature distinct from the
passive background guarded-wipe mechanism — both Force Recheck and the
passive path end up in the exact same wipe/rediscovery logic, gated by the
exact same safety checks. What differs is purely how fast each one can
reach a verdict:

- Force Recheck: 3 probes at normal (1s default) cadence → a verdict in a
  few seconds.
- Passive path: 1 legacy probe every ~45s on an already-confirmed hop,
  needs 2 misses spanning at least another ~45s → **a minimum of about 90
  seconds**, often more, before it even reaches a verdict.

This is a deliberate design trade-off (documented in the code itself,
`kLegacyMissWindowSecs`'s comment) — one probe's timing alone isn't
reliable route-change evidence, several clustered misses (Force Recheck's
burst) or two misses genuinely ~45s apart (the passive path) are. No code
change was warranted here; I documented the exact numbers and the
"why it can look broken if you check too soon" explanation in
`ARCHITECTURE.md` §6 and the CHANGELOG so this doesn't come up as a
surprise again.

**3. GitHub workflows re-verified.** Re-extracted the repo fresh and
re-checked all three modified workflow files
(`tauri-ci.yml`/`tauri-release.yml`/`tauri-canary-build.yml`): all three
parse as valid YAML, the CLI-sidecar build step is present and correct in
each (including the empty-`matrix.rust_target`-falls-back-to-`rustc -vV`
case for Windows/Linux release jobs), naming conventions match
`tauri.conf.json`'s `externalBin`/`deb.files` exactly, and the two OTHER
workflow files in the repo (`obfuscated-build.yml`, `tauri-version-
release.yml`) don't build the Tauri bundle at all, so they correctly don't
need this step. Nothing needed changing here — this was a clean
re-verification.

**4. Documentation updated** (`ARCHITECTURE.md`, `CHANGELOG.md`, this file)
to reflect both findings above.

## 22. Auto-refresh default lowered to 30s + real per-app Settings window

Two asks: lower the passive "auto refresh" frequency's default to 30s, and
add a Settings window (separate window, savable/loadable across restarts)
to change that and other defaults.

**Engine side**: `kHopRecheckSecs`/`kLegacyMissThreshold` were compile-time
constants (45s/2) — turned them into a process-wide runtime default (still
30s/2 out of the box, clamped 5-300s / 2-10 misses) via
`set_default_recheck_tuning()`/`default_recheck_window_secs()`/
`default_recheck_threshold()` (session.hpp/session.cpp), so a change from
the GUI applies live, to every running and future target, with no rebuild.
Added 3 new test cases covering the default value, the clamping at both
ends, and the "<=0 leaves that field unchanged" convention. Full suite +
a fresh ASan/UBSan build both pass clean.

**GUI side**: new Settings window (`Settings → Open Settings…`), a genuine
separate OS window (not a panel), covering the auto-refresh tuning, the
"Add target" form's defaults (probe/trace/timeout/payload/maxhops/
destPort/family/protocol/raw), and the theme. Saved to a JSON file in the
app's config directory, loaded automatically at startup (before any target
can even be added), and broadcast live to the main window on save so its
own form/theme update without a restart.

Plumbing: `netpulse_ffi.hpp/.cpp` → `ffi.rs`/`commands.rs` (`AppSettings`,
`load_app_settings`/`save_app_settings`/`get_recheck_tuning`/
`open_settings_window`) → `tauri-bridge.js` → `SettingsWindow.jsx` (new)
+ `App.jsx` (menu entry, form-hydration effect, live-update listener) →
`main.jsx` (routes `?window=settings` to the new component instead of the
main App — same built index.html, no second Vite entry needed).

**Verification, and what's genuinely different this round**: a real
Rust/Tauri toolchain was actually available in this sandbox for the first
time in this whole engagement — installed the same Linux WebKitGTK/GTK
dev packages tauri-ci.yml installs, then ran real `cargo check`/
`cargo build` against the full Tauri app (Rust host + cxx bridge + all the
new/changed code), which succeeded cleanly with zero warnings. That
surfaced a real, previously-undetectable bug from an earlier round:
`list_interfaces_detailed` (the Interfaces page's data source) had been
registered as a command and granted in `capabilities/default.json`, but
never added to `build.rs`'s own `COMMANDS` list — the actual source of
truth tauri-build reads to generate the permission the capabilities file
grants. Without a real build, this was invisible; fixed now, alongside
proper capability wiring for every new Settings command (including a
second, narrowly-scoped `capabilities/settings.json` for the new window,
rather than just widening the main window's grant to cover it).

The exact `.jsx`/`.js` frontend changes were syntax/parse-verified with a
standalone `esbuild` (the sandbox's registry allowlist blocks `xlsx`'s
CDN-hosted dependency, so `npm ci`/a real Vite build isn't possible here)
— disclosed plainly as the one piece not verified against the real bundler,
same as always when something couldn't be fully verified in this sandbox.

**Docs updated**: `ARCHITECTURE.md` (§3, §6, and the timing-comparison
note from round 21 — all updated from 45s/90s to 30s/60s and to describe
the values as configurable rather than fixed), `cli/CLI.md`, `CHANGELOG.md`,
`README.md` (new "Settings" section), this file.

## 23. Settings window crashing the whole app — root-caused and fixed, this time with a real reproduction

You reported that opening the new Settings window could crash the entire
app, not just fail to open. This round, for the first time, I actually had
a working Rust/Tauri toolchain (`cargo`/`rustc`) available, so instead of
reasoning from source alone I built a real diagnostic:

- Instrumented `open_settings_window` to print which thread it runs on —
  confirmed it's a background thread, never the main/event-loop thread
  (`#[tauri::command]` handlers are dispatched that way by Tauri itself).
- Worked around this sandbox's `xlsx`-CDN network block just long enough
  to run a genuine `npm install`/`vite build` (temporarily pointing the
  `xlsx` dependency at an npm-registry version in a throwaway copy of
  `tauri-app/`, never touching the real `package.json`) and produced the
  actual obfuscated production frontend bundle — the same thing a real
  release build would ship — instead of testing against stale or
  hand-verified JS.
- Ran the real debug binary headless under Xvfb with that real frontend,
  deliberately calling `open_settings_window` from a spawned background
  thread (matching the real dispatch) both before and after the fix.

**Root cause**: `open_settings_window` is the only command in this whole
codebase that creates a native OS window/webview on demand — everything
else is a pure data operation — and it called
`tauri::WebviewWindowBuilder::build()` straight from that background
thread with nothing catching a panic anywhere on the path. Window/webview
creation needs the main thread (GTK) or the right COM apartment thread
(WebView2 on Windows), and an unwinding panic crossing a native callback
boundary is undefined behavior that usually takes the whole process down,
not just the one command — exactly "the whole app crash," rather than a
contained failure. The before-fix build did NOT actually crash under this
sandbox's Linux/GTK stack in my testing (Tauri's own internal cross-thread
dispatch handled it fine there), which fits your report being from Windows
(WebView2's apartment-thread rules are stricter) — I don't have a Windows
target to compile against here, so I couldn't reproduce the exact crash,
only the mechanism.

**Fix** (`tauri-app/src-tauri/src/commands.rs`): window creation is now
explicitly dispatched onto the main thread via Tauri's own
`AppHandle::run_on_main_thread`, and wrapped in `std::panic::catch_unwind`
so any panic during window/webview construction becomes an ordinary error
message shown in the existing "Could not open Settings" popup instead of
taking the app down. The result comes back over a one-shot channel with a
10-second timeout so a stuck main thread can't hang the button forever
either. This isn't a platform-specific patch — it removes the
implicit-dispatch assumption entirely, so it holds regardless of which
platform's specifics actually triggered your crash.

**Verified**: real `cargo build`/`cargo check`/`clippy` (clean), the fixed
binary run headless under Xvfb with the real production frontend bundle
with the window-creation call deliberately exercised off the main thread
(stays up, returns `Ok`, window opens) both before my fix (didn't crash
here, but confirmed the missing-safety-net mechanism) and after (works,
now with the safety net in place), and the full C++ test suite (untouched
this round — this fix is Rust-only) still 100% passing.

**Docs updated**: `CHANGELOG.md` (new "Fixed: opening the Settings window
could crash the whole app" entry), this file.

## 24. Settings moved from a separate window into a tab; investigated the "session.cpp/hpp errors" report

Two things in your last message:

**1. Settings window: blank, and left the process running after you closed
the main window.** Rather than debug the blank-window content-loading path
further (I can't launch a real desktop session in this sandbox to click
through it, so I'd be guessing), I did what you asked instead: moved
Settings out of its own OS window entirely and into a "⚙️ Settings" tab
beside Interfaces, in the same window. This isn't a workaround for either
symptom — it removes the class of bug they both came from: a tab can't go
blank independently of the rest of the app (same page, same JS context,
same webview), and there's no second window left to outlive the main one
when it closes. Removed: `open_settings_window` and everywhere it was
wired in (`lib.rs`, `build.rs`, `capabilities/default.json`), the now-
pointless `capabilities/settings.json`, `SettingsWindow.jsx`, and
`main.jsx`'s `?window=settings` branch. Added: `SettingsPage.jsx` — the
same UI and logic, now just an ordinary tab.

Verified for real: `cargo build`/`clippy` clean, a genuine production
`vite build` of the frontend, and the actual debug binary launched
headless under Xvfb serving that real bundle — I took a screenshot of the
main window (new Settings tab visible beside Interfaces), scripted a click
onto it, and screenshotted again: the full settings form renders correctly,
no blank page. C++ suite untouched and still 100% passing.

**2. "The session cpp/hpp files got many errors in the new gz."** I
extracted both archives you sent (`new-netpulse-fixes.tar.gz`, which
matches what I delivered last round byte-for-byte, and the smaller
`netpulse-fixes.tar.gz`) and diffed `core/src/session.cpp` and
`core/include/netpulse/session.hpp` between them, and against my current
working copy — all three are byte-identical. A real build of that exact
`session.cpp`/`session.hpp` here (GCC, C++20, including a from-scratch
ASan/UBSan build and the full `ctest` suite) compiles with zero errors and
zero warnings. I also checked the Visual Studio project files under
`build/` (CMake-generated, tracking `core/src/*.cpp` correctly) for
staleness and found none. I don't have a Windows/MSVC toolchain in this
sandbox to rule out an MSVC-specific diagnostic these files might trigger
that GCC doesn't — if you can paste the actual compiler error output (or
attach the build log), I'll fix it directly rather than guess at what
"many errors" means.

**Docs updated**: `CHANGELOG.md` (new "Changed: Settings moved from a
separate window into a tab" entry), this file.

## 25. Interface dropdown staleness fixed; tool pages restyled; TCP/UDP lamp report still open

Three things from your last message:

**1. "Add target" source-Interface dropdown missing Wi-Fi (fixed, verified
real bug).** Found it: `App.jsx` fetched the dropdown's interface list
exactly once, on mount, with no polling — an adapter that came up after
that (Wi-Fi joining a network a few seconds after launch, waking from
sleep, a VPN) was invisible until restart, even though the Interfaces tab
(which already polls every 20s for the no-connectivity alert) showed it
correctly the whole time. Fixed by deriving the dropdown list from that
same existing poll instead of a separate one-shot fetch — no new native
call, and it now tracks live adapter state the same way the Interfaces
tab does.

**2. Interfaces/Settings tabs "look like a borderless pasted thing"
(fixed).** Both tool pages used a bare `.toolpage` wrapper with no card
styling anywhere — flat text and inputs directly on the window background,
unlike the rest of the app's `.dcard`-based dashboard. Added a proper
header treatment (bottom border + subtitle), a `.tool-section` card class
for Settings' three groups, and wrapped the Interfaces table in a bordered,
shadowed card with a shaded header row and zebra striping. Verified with a
real production `vite build` and before/after screenshots from the actual
binary running headless.

**3. Lamp/bulb indicators only lighting up for ICMP, not TCP/UDP — still
open, need more from you.** I checked the frontend logic that drives every
lamp/bulb (`hopStatus`, `destLamp`, `pathLamp` in `App.jsx`): all of it is
completely protocol-agnostic, driven only by generic `sent`/`recv`/`loss`
fields the engine reports per hop — nothing in that code branches on
protocol at all. I also ran a real TCP-mode trace via the CLI in this
sandbox against a live target, and the engine correctly populated
sent/recv/loss for the destination hop (0% loss, real RTTs) — so the
mechanism does work at the engine level when replies come back. I can't
reproduce your exact symptom here: this sandbox's network is a single-hop
proxy setup, nothing like a real multi-hop route on your machine, and I
have no Windows toolchain to test Windows-specific behavior. My best
guess is this is the SAME class of thing the engine already has a named
mechanism for — intermediate routers' ICMP Time-Exceeded errors not making
it back to a TCP/UDP probe's socket on some networks/OSes, which the app
already detects and surfaces as a `⚠ protocolHint` warning under the
target after ~25s. If you can tell me: (a) does that yellow warning appear
on a TCP/UDP target, and (b) is it just the intermediate hop bulbs that
stay gray, or does the destination's own two-lamp indicator (top of the
target card) also never turn green — that will tell me whether this is
the known ICMP-delivery limitation or a real, different bug I still need
to find.

**Docs updated**: `CHANGELOG.md` (two new entries — interface dropdown fix,
tool-page styling), this file.

## 26. Found and fixed the real TCP/UDP hop-lamp bug (sent=0 hops rendering green); interface "kind" (Wi-Fi/Ethernet) added

Your latest screenshots gave me exactly what I needed: the TCP:443 trace
to 1.1.1.1 showed hops 4-9 at `SENT=0, RECV=0` — never even probed yet —
still rendered with a plain green dot, same as a real reply. That's not
the ICMP-delivery-hint scenario I guessed at last round; it's a straight
bug in the frontend's status logic.

**Root cause**: `hopStatus()` and friends (`isDiscovering()`, `destLamp()`,
`pathLamp()`, `targetState()` — all in `App.jsx`) defined "no reply" as
`sent > 0 && recv === 0`. That's correct for "asked, got nothing", but a
hop that was never asked at all (`sent === 0`) fell through every check to
the same `'ok'`/green default a real healthy hop gets. TCP/UDP hit this
constantly because hop-discovery only probes a TTL once discovery reaches
it; ICMP probes every TTL up front, so it rarely showed the gap — which is
exactly the TCP-vs-ICMP difference you flagged.

**Fix**: `sent === 0` is now checked first everywhere, and unprobed hops
get their own `'pending'` state — a hollow ring, not a filled dot — instead
of collapsing into green. Verified live: I added an ICMP target in this
sandbox and caught it mid-discovery with hop 1 real (green), hop 2
unprobed, hop 3 probed/100% loss — the three now render as three visibly
different dots (filled green / hollow ring / filled grey). Same code path
drives TCP and UDP identically, so this fixes those too.

**Interface dropdown, part 2**: the fix from last round (polling
`/api/interfaces/detailed` every 20s) was already correct — I re-verified
the filter logic matches the C++ side's own `list_interfaces(false)`
exactly. Since I couldn't reproduce "still not visible" without your
environment, I read "add more details here" as wanting a way to actually
tell adapters apart, and added a best-effort adapter `kind`
(Wi-Fi/Ethernet/Virtual) — new C++ detection in `transport.cpp` (Windows:
`IfType`; Linux: `/sys/class/net/*/wireless`/`phy80211`), threaded through
to the dropdown (`[Wi-Fi] name — address`), a new "Kind" column on the
Interfaces tab, and a matching column in the CLI's `ifconfig`. Also added
a manual refresh button next to the dropdown so a just-connected adapter
doesn't need to wait out the 20s poll.

**Verification**: real CMake build + `ctest` (core, all green), `cargo
check` (Rust side, clean), a real `vite build` production bundle, and a
full Xvfb-headless run of the actual debug binary — screenshotted the
Interfaces tab (new Kind column showing "Ethernet"), the Add-target
Interface dropdown (refresh button in place), and a live target mid-
discovery showing the three distinct hop-dot states described above.

**Still open, not re-raised this round**: the original "session.cpp/hpp
many errors" report — still waiting on the actual MSVC compiler log,
since I have no Windows toolchain here and couldn't reproduce any error
with GCC/Clang/ASan/ctest.

**Docs updated**: `CHANGELOG.md` (three new entries), this file.

## 27. Fixed: Interfaces/Settings tabs unscrollable on a small window (this was a genuine regression from round 25's styling pass)

Your screenshot showed it clearly: on a small window, Settings cut off
right at "APPEARANCE" with nothing below it reachable, and the Interfaces
table was missing MTU/Egress entirely. Both were real bugs I introduced in
round 25's "make it look advanced, professional" styling pass:

- `.toolpage` (every tool page's root) never had any `flex`/`overflow`
  rules. It sits as a direct sibling of the tab bar inside `.app`, which is
  a fixed-height flex column with `overflow: hidden`. Without
  `flex: 1 1 auto; min-height: 0; overflow-y: auto` on `.toolpage` itself,
  a page taller than the remaining window height just got clipped by the
  parent — there was nothing to scroll. (`min-height: 0` specifically is
  the fix for a subtle flexbox default: a flex item won't shrink below its
  content's natural height unless you say so.)
- `.iface-table-wrap`'s `overflow: hidden` (added purely to keep rounded
  corners) clipped the whole right side of the table on a narrow window —
  MTU and Egress weren't missing from the data, they were rendered and cut
  off. Switched to `overflow-x: auto` and gave the table a real
  `min-width` so it actually triggers a scrollbar instead of just
  shrinking every column illegibly.

Also finished "add more details" properly this time: a new best-effort
**Kind** column (Wi-Fi/Ethernet/Virtual/Other) on the Interfaces table,
the interface dropdowns, and the CLI's `ifconfig` — detected via adapter
IfType on Windows and `/sys/class/net/*/wireless`|`phy80211` on Linux.

**Verification**: rebuilt the frontend for real (`vite build`, clean), and
ran the actual debug binary headless at the app's own configured minimum
window size (1280×720 — Tauri's `minWidth`/`minHeight`) to confirm this
wasn't just a CSS theory: screenshotted the Settings tab scrolled down to
the Appearance section and Save button (both now reachable, visible
scrollbar), and the Interfaces table showing MTU/Egress at full width.

**Docs updated**: `CHANGELOG.md` (new entry), this file.

## 28. Fixed: Interfaces table boxed into a fixed-width column with its own scrollbar instead of using the real window width

Your screenshot showed the actual problem clearly: the table was stuck in
a narrow column with a horizontal scrollbar, and everything to the right
of that column — most of the window — was just empty. That was on me:
round 27 gave every tool page's root `.toolpage` a `flex`/`overflow-y`
fix for the vertical-clipping bug, but never questioned the existing
`max-width: 900px` on that same class — fine for Settings/DNS/Ping (forms
read better at a fixed width), wrong for a table, which should just use
whatever width the window actually gives it. I'd also added an explicit
`min-width: 660px` to the table itself in that same round, which made it
need horizontal scrolling even more eagerly than necessary — redundant on
top of `white-space: nowrap`, which already stops any cell from getting
squeezed illegibly.

**Fix**: Interfaces now renders with a new `.toolpage.fluid` variant
(`max-width: none`) instead of the generic capped one, and the redundant
table min-width is gone. Settings/DNS/Ping are untouched — they're forms,
and a fixed reading width is the right call there.

**Verification**: real `vite build`, then a headless run of the actual
binary at both a wide window (1900×1000 — table now spans the full width,
no dead space) and the app's own configured minimum (1280×720 — still
fits cleanly, no unnecessary scrollbar).

**Docs updated**: `CHANGELOG.md` (new entry), this file.

## 29. Fixed: Settings tab had the exact same fixed-width-box problem as Interfaces

You were right, I'd only fixed half of it. `SettingsPage.jsx` had the same
`"toolpage"` (900px cap) PLUS its own inline `maxWidth: 640` stacked on
top — same dead-space problem as the table, just without a scrollbar to
make it obvious.

**Fix**: Settings now uses `"toolpage fluid"` too (no width cap), and the
field grids switched from a fixed 2-column layout to
`repeat(auto-fit, minmax(200px, 260px))`, so more fields show per row as
the window widens instead of leaving space unused — 6 per row at 1900px,
4 at the app's minimum 1280px, 1 on anything narrower.

**Verification**: real `vite build`, then a headless run confirming both
window sizes look right and the vertical-scroll fix from two rounds ago
is still intact.

**Docs updated**: `CHANGELOG.md`, this file.

## 30. Fixed CI failure on all three OSes: stale committed CMakeCache.txt with your local Windows path baked in

Your CI logs showed it clearly: `build-cli-sidecar/CMakeCache.txt` was
recorded against `c:/Users/Archisman/Downloads/NetPulse-cpp-web/33/
NetPulse-cpp-web/build-cli-sidecar` — your own local machine's absolute
path — and that exact file is what every CI runner (Windows, Linux, AND
macOS all showed the same local Windows path) inherited from a fresh
clone. CMake correctly refuses to reuse a cache recorded against a
different location, so `build.rs`'s CLI-sidecar step panicked before
compiling anything, on all three platforms.

**Root cause**: `.gitignore` only had `build/` (an exact match), which
doesn't cover `build-cli-sidecar/` — so that directory's CMake cache
wasn't excluded and ended up committed.

**Fix**:
- `.gitignore`: `build/` → `build*/` (directory-only glob, so it can't
  touch the `build-and-run.sh`/`.ps1` files) — covers `build-cli-sidecar`,
  `build-cli`, `build-verify`, `build-asan`, and any future one, in one
  line instead of a new entry every time this happens.
- `build.rs`: now checks a cache's own recorded source directory against
  the current one before trusting it, and wipes + reconfigures from
  scratch on a mismatch instead of handing CMake something it will refuse.
  This makes the build self-healing even if a mismatched cache shows up
  again some other way.

**Still needed on your end**: I can't push to your actual GitHub repo
from here, so the already-committed `build-cli-sidecar/CMakeCache.txt`
needs removing in a real commit —
`git rm -r --cached build-cli-sidecar` (and any other stray `build*` dir
`git status` shows tracked), then commit and push. CI should actually
recover even before you do that, thanks to the `build.rs` self-heal, but
the stale file shouldn't stay in history.

**Verification**: reproduced the exact failure locally by planting a
`CMakeCache.txt` with a bogus recorded path and forcing a rebuild —
confirmed it panicked before the fix, and `cargo build`/`cargo check`
complete cleanly with a real working sidecar binary produced after it.

**Docs updated**: `CHANGELOG.md`, this file.

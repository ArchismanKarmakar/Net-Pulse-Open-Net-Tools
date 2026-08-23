# Changelog

## 1.0.8

### macOS build: missing AudioToolbox framework link

The very first real macOS CI build of this codebase (obfuscated-build.yml)
failed at the link stage: `Undefined symbols for architecture arm64:
_AudioServicesPlaySystemSound`. The core engine itself linked fine on macOS
(its own CI job passed) — this was isolated to `netpulse_ffi.cpp`'s
`play_alert_sound()`, whose macOS branch calls `AudioServicesPlaySystemSound`
(declared correctly via `<AudioToolbox/AudioServices.h>`, compiling without
complaint) without `build.rs` ever telling the linker about the
`AudioToolbox` framework it lives in. Tauri's own build links AppKit,
WebKit, Security, and several other frameworks it needs for its own
windowing/webview — none of which happen to pull in AudioToolbox as a side
effect, since nothing else in the binary uses it. This project's own
build.rs has to link the frameworks its own code needs explicitly, and
never did for this one — this exact code path had no macOS cross-compiler
available while it was being written, so this was the first time it was
ever actually compiled for the platform. Fixed by adding the missing
`cargo:rustc-link-lib=framework=AudioToolbox` directive specifically for
`cfg!(target_os = "macos")`, distinct from the generic Unix branch Linux
also falls into (which correctly needs no such framework at all, since its
own `play_alert_sound()` branch is a deliberate no-op).

Two more warnings visible in that same build log — unused `discovering`
(session.cpp) and an orphaned `kDirectEchoTtl` constant, both flagged by
clang's warning set but not GCC's, which is what `verify.sh`'s own
warning-budget check runs against — were confirmed genuinely dead (not a
functional gap: each `ProbeStrategy`'s own `send_direct_probe()` already
independently hardcodes TTL 255 with the identical reasoning comment) and
removed.

### Release pipeline: entire release blocked on an updater signing key that doesn't exist yet

With the macOS build fixes above landing, the first real release run (all
three OS installers built successfully) still failed — this time in the
publish job, at "Merge per-OS updater manifests into one latest.json": zero
`latest-*.json` manifests were found among the downloaded artifacts. Root
cause, and confirmed by `CODE_SIGNING.md`'s own heading ("future
implementation") and text ("nothing here is wired up yet"): this project
genuinely does not have a `TAURI_SIGNING_PRIVATE_KEY` configured yet —
`createUpdaterArtifacts` correctly refuses to produce a `latest.json`/`.sig`
on any platform without one, since an unsigned update manifest can't be
trusted. That's expected, not a bug — but the merge script unconditionally
hard-failed the whole job whenever it found nothing to merge, which meant
this correct, documented "not set up yet" state was blocking **every
release from ever publishing**, installers included, even though the
installers themselves build and work completely fine without a signing key
at all. Confirmed none of the downstream steps (checksums, GPG signing,
build-provenance attestation, the GitHub Release itself) actually require
`latest.json` to exist — they all operate generically on `release/*`.
Fixed by mirroring the job's existing `HAS_GPG_KEY` pattern (secrets can't
be read in a step-level `if:`, so the one bit needed — "is a key
configured" — is decided once at job level instead) into a new
`HAS_SIGNING_KEY`, and having the merge script tell apart two genuinely
different situations: no key configured at all (expected — warn and exit
0, installers still publish, just without auto-update) vs. a key IS
configured and still produced nothing (a real, unexpected problem — e.g. a
malformed key — still exits 1 exactly as before). Verified all three paths
execute correctly (no-key-skip, key-configured-failure, and the normal
merge-succeeds case) before trusting it — and caught a real typo of my own
along the way, a malformed escape sequence that would have made the script
a syntax error on every future invocation of the failure path.


The same build log also showed the final link command using
`-mmacosx-version-min=11.0.0`, not the `10.15` `build.rs` sets via
`MACOSX_DEPLOYMENT_TARGET` — a real, separate bug from the AudioToolbox one
above. Root cause: `build.rs`'s `std::env::set_var(...)` only ever affects
`build.rs`'s own process and whatever it spawns itself (`cc`-rs's clang
invocations, compiling the C++ engine) — it cannot reach backward to affect
Cargo's own, separate `rustc` invocation for the actual link step, since
that's a sibling process Cargo spawns directly, not a child of `build.rs`.
Every macOS build had silently been linking against whichever deployment-
target default the Rust target triple happens to have (11.0 for
`aarch64-apple-darwin`, since ARM64 Mac hardware never existed before that
version) rather than the `10.15` `tauri.conf.json`'s own
`macOS.minimumSystemVersion` claims the app supports — and, for the release
workflow's universal binary specifically, meant the x86_64 and aarch64
slices within the same binary could disagree on their own deployment
targets, not just disagree with the app's stated one. Fixed by setting
`MACOSX_DEPLOYMENT_TARGET` in each CI workflow's own job-level `env:` block
(`obfuscated-build.yml`, `tauri-ci.yml`, `tauri-release.yml`,
`tauri-canary-build.yml`) — the actual top of the process tree, correctly
inherited by both Cargo's link step and `build.rs`. `build.rs`'s own
fallback still matters for a local build invoked without that env block
already set; its comment now says so explicitly rather than calling itself
"the single source of truth", which this real build proved it wasn't.

### Single source of truth for the version number

`/VERSION` at the repo root is now the one canonical version string, with
`scripts/sync-version.mjs` propagating it into the three files that each
need their own literal copy (`tauri-app/src-tauri/tauri.conf.json`,
`tauri-app/src-tauri/Cargo.toml`, `tauri-app/package.json` — none of Cargo,
npm, or Tauri's bundler support reading their `version` field from an
external file, so each still needs it written in directly). `node
scripts/sync-version.mjs` rewrites all three; `--check` verifies they
already match without changing anything, which is what CI now calls instead
of duplicating the same three-way comparison inline. `tauri-version-
release.yml` was updated to watch `/VERSION` (not `tauri.conf.json`) and to
call the script's own `--check` rather than re-implementing the same
consistency logic a second time — two copies of that logic drifting apart
from each other was exactly the kind of thing this system exists to
prevent.

### Path/MTR and Ping: TCP and UDP measurement accuracy

A long series of root-caused, individually-verified fixes to the actual
correctness of TCP/UDP/HTTP measurement, most surfaced only once the app
was exercised on real hardware and real networks rather than a sandboxed
loopback:

- **TCP/HTTP RTT quantization (100ms floor).** `run_tcp`/`run_http` sent a
  probe, then unconditionally blocked up to 100ms on the ICMP inbox before
  ever checking `poll_completions()` — but TCP/HTTP replies never arrive
  through that inbox at all, so every pass measured the sleep, not the
  network. Both a LAN router and 8.8.8.8 read a suspiciously flat ~100ms.
  Fixed with an adaptive wait (2ms while a probe is in flight, 100ms when
  idle). The identical bug independently existed a second time in the
  standalone Ping tool's own engine (`ping_run.cpp`) and was missed on the
  first pass — found later from a live report showing 8.8.8.8 and 1.1.1.1,
  genuinely different real RTTs, both reading an identical ~50ms; fixed the
  same way there.
- **TCP/HTTP hop-correlation bug (unchecked `bind()`).** On Windows, `bind()`
  to an ephemeral port in the Hyper-V/WSL2-reserved range (49152+) can fail
  (`WSAEACCES`) — the original code never checked this, so the probe left
  from a random port the router's Time-Exceeded didn't match, and
  intermediate hops silently never resolved even though the destination
  still worked. Fixed by confirming the actual bound port via
  `getsockname()` *after* `connect()` and correlating on that instead of
  arithmetic.
- **HTTP negative RTT (`-0.1ms`).** `poll_completions()` used the caller's
  `now` (captured before the probe was even sent) instead of a fresh
  timestamp, so `now - sent_at` could go negative. Fixed to compute `now`
  fresh, matching how the TCP path already did it.
- **ICMP MTR hop starvation after sleep/wake.** The already-answered-hop
  keepalive phase iterated hops in a fixed, non-rotating order under a
  shared global pacer; hop 1 could consume every available token every
  pass, starving hops 9+ entirely under contention. Fixed with a rotating
  cursor for that phase, mirroring the one the unanswered-hop phase already
  had.
- **Monotonic clock for RTT.** TCP/HTTP RTT and timeout comparisons used
  wall-clock (`system_clock`), vulnerable to NTP corrections and sleep/wake
  jumps corrupting every in-flight probe's measured time. Split into a
  wall-clock timestamp (kept for the reported sample time) and a separate
  monotonic one (`steady_clock`) used only for RTT/timeout math.
- **Windows raw-socket ICMP delivery.** Root cause of TCP/UDP hop discovery
  never working on Windows at all: a raw `SOCK_RAW`/`IPPROTO_ICMP` socket
  there does not receive ICMP errors generated in response to a *different*
  socket's traffic (the router's Time-Exceeded for a TCP/UDP probe never
  reaches it) unless the socket is both bound to a real local address *and*
  put into `SIO_RCVALL` receive-all mode. Implemented, logged either
  outcome explicitly (`SIO_RCVALL enabled`/`FAILED`) so this is diagnosable
  rather than a silent dead end, and — after an earlier version incorrectly
  claimed this doesn't work for IPv6 with no actual source for that
  restriction — extended to IPv6 too, since the ioctl itself doesn't encode
  an address family and the call is already fully best-effort.
- **UDP-mode port-allocation collision (the most severe of these).**
  `allocate_flow_port_block()` handed out one of only 64 fixed port blocks
  via a blind, process-wide, never-resetting round-robin counter — every
  MTR target and every Ping run consumes one call, so 64 *cumulative*
  allocations (not 64 simultaneous ones, which the original code's own
  comment incorrectly claimed) is trivially reached over a real working
  session. Once the counter wrapped, a brand-new session could be handed
  the exact same block as a still-running one, silently overwriting its
  ICMP-reply-routing registry entries — explaining a live report of UDP
  ping working for one target and then completely failing for others
  moments later, unrelated to the actual destination. Fixed by checking
  registry occupancy before committing to a candidate block, falling back
  to the old round-robin choice only if every one of the 64 is genuinely
  occupied. Verified against the *actual* pre-fix code, not just reasoned
  about: an executable test simulating realistic sustained use (a
  long-lived session plus 80 short-lived ones cycling through) reproduces
  a real collision at exactly call #63 against the old allocator and shows
  zero collisions against the fixed one.
- **UDP base-port collisions with real services.** Choosing an unusually
  low UDP base port (e.g. 50) means a mid-range hop offset can land on a
  well-known service port (50+3 = 53, DNS) that has something genuinely
  listening — the destination silently discards the malformed non-DNS
  payload instead of replying with Port-Unreachable, which looks like a
  bug but is a direct, predictable consequence of not using an
  intentionally-obscure base port the way classic `traceroute` does.
  Confirmed by reproducing the exact scenario against a real listener on
  that port.
- **Stale, permanent "N automatic reconnect attempts" banner.** The counter
  behind this banner (`silence_rebuild_count_`) only ever incremented and
  was never reset anywhere, despite existing specifically to answer "is
  this session actively retrying right now" — a session that briefly lost
  ICMP (e.g. a VPN toggle) and then fully recovered kept the banner pinned
  at its outage-time value forever, contradicting hop data that was
  simultaneously showing a fully healthy path. Fixed by resetting it
  alongside the existing, already-correct local counters it should have
  mirrored from the start. Verified live: genuinely blocked ICMP via
  `iptables`, watched the counter climb, removed the block, watched it
  reset to zero within seconds of a real reply.

### Ping tool: TCP and UDP added, NMAP-style reply detail, validation

- **TCP and UDP ping**, alongside the existing ICMP mode — new
  `PingProtocol::Tcp`/`Udp` in the shared `PingRun` engine, wired through
  the full stack (native FFI, Rust command, JS bridge, UI). TCP ping needs
  no elevation or capture driver on any platform (the SYN-ACK/RST answer
  arrives on the TCP socket itself, not as an ICMP message); UDP ping's
  reply is an ICMP Port-Unreachable, which does need elevated privileges to
  receive.
- **NMAP-style reply detail.** A TCP reply used to render with the exact
  same generic "Reply from…" text ICMP uses, even though the underlying
  evidence is completely different — the C++ layer already distinguished a
  completed handshake from a refused connection internally and was simply
  discarding that distinction before it reached the UI. Now shown plainly:
  "Connected (port open)" vs. "Port closed, but host responded (RST) — host
  is reachable".
- **Local/remote port shown in every reply and timeout line**, both TCP and
  UDP, including on a timeout (previously a `Request timed out` line
  carried zero identifying detail — exactly the missing piece needed to
  diagnose the base-port-collision issue above from a log alone).
- **Full input validation before starting a probe.** Count/Size/Timeout/
  Interval/TTL/remote-port only ever had soft HTML5 `min`/`max` hints,
  which do not actually stop the browser from using an out-of-range typed
  value — nothing validated them before launching. Now checked against
  explicit limits before every run, blocking with a native warning dialog
  (and the OS alert sound) listing every problem at once rather than
  starting with silently-clamped or nonsensical values.

### Diagnostic logging

A new, opt-in file-based logging system for exactly the class of bug this
whole line of fixes came from — the state that's easiest to misdiagnose
from the UI alone.

- **Toggle lives in Tools → Diagnostic logging**, not an environment
  variable: `NETPULSE_DEBUG=1` cannot be set for a "Run as Administrator"
  relaunch at all (a fresh elevated process inherits none of the launching
  terminal's environment), which is exactly the scenario most in need of
  diagnosing. The file-based toggle works no matter how the app was
  started.
- **Persistent file handle, not fopen/fclose per line.** The very first
  version of this reopened the log file on every single call — including
  from the shared RX dispatcher's hot path, hit by every incoming packet
  across every active session. Over a long session with several targets,
  that's tens of thousands of raw file-open syscalls, and on Windows
  specifically each one is a real-time-antivirus-scan hook opportunity.
  Fixed with one handle kept open and flushed (not closed) after each
  write, plus a 20MB size cap so a long session degrades to "logging
  stops" rather than filling the disk.
- **UTF-8 BOM + timestamps.** A real log file showed messages like `SIO_
  RCVALL enabled ΓÇö raw socket...` — the classic signature of a correct
  UTF-8 em-dash being decoded as CP1252 by a viewer with no other signal
  about the file's encoding. Fixed by writing a UTF-8 BOM as the first
  bytes of a newly-created log file (written exactly once, verified not to
  duplicate across a disable/re-enable cycle), and every line now carries a
  `[HH:MM:SS.mmm]` timestamp automatically.
- **NMAP-style "filtered" logging on every timeout**, across TCP/UDP/HTTP
  hop discovery, with the specific hop, target, and local/remote port —
  the same detail level added to the Ping tool's own timeout lines.

### Protocol prerequisite detection and prompts

- **Real capability detection**, not guesses: `process_is_elevated()`
  (Windows token check / `geteuid()`) and `capture_driver_present()` (tries
  to actually load `wpcap.dll`, including Npcap's non-default search path
  — proving the driver is genuinely usable now, not just that a registry
  key an uninstall could have left behind still exists).
- Selecting UDP or TCP now checks prerequisites **immediately**, before a
  host is even typed, rather than only at "Add" time; "Add" still
  re-checks authoritatively as a safety net, since capabilities can change
  between selection and click (elevating in another window, installing
  Npcap).
- **"Restart as Administrator"** actually restarts the app elevated
  (PowerShell `Start-Process -Verb RunAs`, chosen specifically to avoid a
  new `windows-sys` dependency); declining the UAC prompt leaves the
  running instance untouched rather than treating it as a crash.
- Every prerequisite/IP-translation/update dialog is genuinely blocking —
  an earlier version could be dismissed by an accidental outside click,
  which for two of the IP-translation dialogs meant the click silently
  triggered the same *fallback-and-proceed* path a deliberate button choice
  would have, rather than actually canceling. Reworked to three explicit
  outcomes (Cancel / a real fallback choice / the primary action) with an
  executable test proving all four ways of resolving the dialog (including
  the dialog's own × close button) do what they're supposed to.

### In-app modal system

The generic confirmation dialog used throughout the app went through
several real, found-and-fixed bugs of its own while being extended:

- **The modal was never actually rendered at all.** `showModal()`/
  `closeModal()` worked by resolving a Promise only when a rendered
  button's `onClose` ran — but the `<Modal>` component itself was defined
  and never once placed in the render tree. Every one of the ~30 call
  sites across the app hung forever, silently, with no error — this is
  what a live report of "TCP/UDP alerts never appear, and clicking Add
  does nothing" turned out to be, unrelated to protocol logic entirely.
- **Missing CSS.** Even after fixing the render, none of `.modal-overlay`/
  `.modal-box`/etc. had a single CSS rule anywhere — an unstyled block in
  normal document flow, effectively invisible. Both had to be fixed
  together; fixing only one would have looked identical to the original
  bug.
- **Constant remount ("flashing").** `Modal` was declared *inside* the
  `App()` component body — a new function value on every single App
  re-render, which happens continuously while a session streams live data.
  React treats a changed component identity as "unmount and remount",
  invisible before any CSS entrance animation existed, but very visible
  once one did: every remount replayed the open animation. Fixed by
  hoisting it to module scope (it never closed over anything from `App`'s
  own scope, so this was always safe). Hoisting it also surfaced a second,
  pre-existing bug: a stale, never-actually-used duplicate `Modal.jsx` file
  had been silently shadowed by the in-component one this whole time —
  deleted rather than left as a second, drifting copy.
- **No close animation.** The close transition reused the *same*
  `@keyframes` name as the open one with `animation-direction: reverse` —
  once an animation with a given name has already finished on an element,
  changing only its direction/duration via a class swap does not reliably
  restart it in Chromium/WebView2; the element just sat at its settled end
  state until JS removed it. Fixed with genuinely distinct keyframe names
  for open vs. close, which forces a real restart. The delayed-unmount
  timing this depends on (keep the element mounted long enough for the CSS
  transition to actually play) had its own subtle bug: resolving the
  dialog's Promise *before* that delay let a caller show a follow-up dialog
  mid-animation, which the first dialog's own pending cleanup would then
  incorrectly null out from under it — proven with an executable
  reproduction before and after the fix, not just reasoned about.
- **× close button**, always available even on a blocking dialog (a
  deliberate click is not the accidental-outside-click case blocking
  exists to guard against), and the same animation/blocking treatment
  extended to the separate About dialog.
- **Native OS alert sound**, decoupled from showing a native OS dialog box:
  `play_alert_sound(kind)` plays only the platform sound (Windows
  `MessageBeep`, tiered info/warning/error; macOS `AudioServicesPlay
  SystemSound`; an honest no-op on Linux, which has no single standard
  API across desktop environments) with no dialog shown at all — added
  specifically so the app's own styled, resizable modal could get the same
  attention-getting sound a native dialog gets for free, without giving up
  control over the dialog's own appearance (a native OS dialog's text size
  cannot be influenced by the app at all). Along the way, found and removed
  an entirely separate, ad-hoc synthesized WebAudio beep `showModal()` had
  always played on every call — meaning some dialogs were briefly playing
  two unrelated sounds simultaneously once the real OS sound was added
  alongside it.
- **Semantic button colors** — teal/confirm, amber/warning, burnt-orange/
  danger, plain/neutral — replacing a single color used for every action
  regardless of how consequential it was. Two rounds of real fixes here:
  the first color choice used bright, high-saturation fills that read as
  glare next to white text even though they numerically passed WCAG
  contrast; darkened and unified across both themes. Separately, a CSS
  specificity bug (`.modal-actions button`, an element+class selector,
  unconditionally beating a single-class `.btn-warning`/`.btn-danger`
  regardless of source order) meant the color frequently didn't render at
  all inside any modal — the exact same specificity trap had already been
  found and fixed once for the primary/confirm button, but not extended to
  its two siblings until a live screenshot showed neither had any color.

### Path/MTR: Edit Config, NMAP-style port state, port visibility

- **Remote port is now editable** in Edit Config (previously read-only
  display text). Session::run_tcp()/run_udp()/run_http() were found to
  never consult the rebuild mechanism a settings change is supposed to
  trigger at all — only the ICMP-mode loop ever did — so a live in-place
  port change would have silently done nothing useful while leaving stale,
  port-specific correlation state behind. Rather than hand-write a new
  in-place rebuild for three separate loops under time pressure, a port or
  protocol change is routed through the already-correct, already-tested
  remove-and-re-add path instead, with an explicit warning (hop history for
  that target is genuinely lost, not resumed) and confirmation before it
  happens.
- **NMAP-style open/filtered/closed** shown per hop for TCP mode, using the
  same SYN-ACK/RST distinction added to the Ping tool, threaded through to
  the destination hop specifically (only the real endpoint can ever answer
  with a genuine TCP-layer reply — an intermediate hop's only possible
  evidence is an ICMP Time-Exceeded, which has no open/closed concept at
  all).
- **Local port surfaced as a per-hop tooltip**, not a single CONFIG-bar
  summary value — a summary showing one hop's value implied a single,
  session-wide answer that doesn't actually exist for TCP (a fresh socket
  per probe, by design) and could go stale for UDP after a silence-
  rebuild reassigns the session's shared socket. The per-hop tooltip reads
  directly from that specific hop's own latest recorded value and can't be
  misleading the same way.

### Resource lifecycle and long-session stability

- **`ColdStore`'s background persistence queue had no size cap at all.**
  One thread services every target's cold-tier flush jobs; if disk I/O
  ever fell behind generation rate (a slow disk, or real-time antivirus
  scanning intercepting every write), jobs piled up in memory indefinitely.
  Capped with oldest-drop-and-log rather than blocking, since blocking
  would stall live probing, which is worse than losing some historical
  detail.
- **Removing a target never cleaned up its `ColdStore` state.** Neither the
  in-memory compute cache nor the on-disk history files for a removed
  target were ever released — invisible in a short test session, a real
  problem for a long-running deployment where targets get added and
  removed over days or weeks, since both grow with every target *ever*
  added rather than every target currently active. Fixed with a proper
  `forget_target()`, wired into removal and verified directly (pushed
  data, confirmed it existed in both cache and on disk, removed it,
  confirmed both were gone).

### Deployment and cross-platform build parity

A full read-through of the GitHub Actions release pipeline, none of which
had been directly audited before — several real, concrete gaps found:

- **macOS builds were architecture-incomplete.** The release workflow
  specified no target at all for the macOS job; `macos-latest` runners are
  Apple Silicon, so with nothing else specified the build produced an
  ARM64-only binary — not "runs slower on Intel via Rosetta", genuinely
  unable to run there at all, since Tauri does not produce a universal
  binary unless explicitly told to. Fixed by installing both Rust targets
  and building with `--target universal-apple-darwin`.
- **The documented "rebuild an existing tag" workflow_dispatch input was
  silently broken.** Neither checkout step in the release workflow
  specified a `ref`, so triggering a rebuild for an old tag would have
  silently built whatever `main` currently looks like instead of the
  actual historical tagged commit. Fixed on both the build and publish
  jobs.
- **No `.icns` file existed anywhere** for the macOS bundle icon — absent
  from disk and from `tauri.conf.json`'s icon list. Generated one from the
  largest available source PNG.
- **A referenced-but-never-created workflow.** `Cargo.toml`'s own doc
  comment on the `canary` feature (a devtools-enabled debug build) named
  `tauri-canary-build.yml` as how to produce one — that file never existed,
  despite the feature flag and its dedicated Tauri config
  (`tauri.canary.conf.json`) being real, correct, and already wired up.
  Created it: manual-dispatch only, uploads artifacts rather than
  publishing a Release (a debug/devtools build should never be mistaken
  for a real release), same universal-macOS-binary treatment as the real
  release workflow.
- **Verification suite (`scripts/verify.sh`) expanded** to cover several
  classes of bug found the hard way during this work and unlikely to be
  caught by ordinary code review: every `core/src/*.cpp` and
  `netpulse_ffi.cpp` compiled standalone (catches a missing `#include`
  masked by a different file in the same static-library link happening to
  provide it — this exact bug shipped once), the same files cross-compiled
  for Windows via MinGW-w64 (catches Win32-only API/header mistakes that
  native Linux compilation can't see at all — including a Windows-only
  `winsock.h`/`winsock2.h` header-ordering conflict this caught directly),
  a three-way consistency check across every Tauri command's registration
  in `lib.rs`/`build.rs`/`capabilities/default.json` (a command missing
  from any one of the three either fails the whole build or is silently
  uninvokable at runtime with no compile error at all — both happened
  during this work before this check existed), every workflow YAML file
  parsed for validity, and version consistency across `VERSION`/
  `tauri.conf.json`/`Cargo.toml`/`package.json`.

## 0.9.5 revised

The work below shipped as part of the 1.0.8 release above rather than
getting its own dedicated version bump.

### Ping tool: rebuilt on the native engine, not an OS subprocess

The standalone Ping tab used to spawn and text-parse the OS `ping` binary —
the one tool in the app that didn't share the main engine's pooled-socket
architecture. Replaced end to end:
- **New engine (`core/include/netpulse/ping_run.hpp`, `core/src/ping_run.cpp`):**
  `PingRun`, a second `IcmpOwner` implementation alongside `Session` (see
  `ARCHITECTURE.md` §2), sharing the exact same pooled sockets and RX
  dispatcher thread — no separate thread, no subprocess. The cross-session
  registry (`g_registry`) was generalized from `map<uint16_t, Session*>` to
  `map<uint16_t, IcmpOwner*>` to make this possible.
- **Manager orchestration:** `start_ping`/`stop_ping`/`poll_ping`, mirroring
  the existing target lifecycle but short-lived (auto-cleanup once done and
  drained).
- **Frontend (`PingPage.jsx`):** now consumes structured per-line results
  (`{seq, ok, rtt_ms, from, note}`) instead of regex-parsing raw process
  output text.
- **Fixed during rollout:** the new `ping_run.cpp` was missing from
  `tauri-app/src-tauri/build.rs`'s hand-maintained `cxx_build` file list
  (unlike `CMakeLists.txt`, which globs `core/src/*.cpp` and picked it up
  automatically — masking the omission through every C++-only test run).
  Surfaced as a real MSVC link error (`unresolved external symbol
  PingRun::PingRun`/`PingRun::run`) the first time the actual Tauri/Cargo
  build ran on real hardware.

### Drag-to-reorder: replaced, not patched again

The sidebar/dashboard target list's drag-to-reorder was hand-rolled on raw
`mousemove`/`mouseup` events with a live-DOM-sibling-query + `busy`/
`requestAnimationFrame` gate — after two rounds of patching the same race
condition kept resurfacing under fast multi-card drags. Replaced entirely
with [Motion](https://motion.dev/)'s `Reorder.Group`/`Reorder.Item` (pointer-
gesture based, not native HTML5 drag-and-drop — irrelevant to Tauri's
webview intercepting native `dragstart`, which is why this was hand-rolled
in the first place). `useDragControls()` per row (via a small wrapper
component, since it's a hook and can only be called once per rendered item)
keeps dragging restricted to the existing `⠿` handle. The custom FLIP-
animation hook is gone — Motion's `Reorder.Item` animates displaced rows
natively.

### Light-mode contrast fix

`.tc-dest` (the destination/family/cadence line on each sidebar card) used a
fixed Tailwind gray token (`--color-gray-300`) instead of the theme-aware
`--muted` variable every other muted-text rule in the stylesheet uses — a
stray typo, per the code's own adjacent comment stating the intended value.
Unreadable against a light background. Fixed to `var(--muted)`.

### Exports: Full CSV, multi-sheet Excel, and Traceroute PNG

- **Full-history CSV** (per target and fleet-wide) — every recorded sample,
  not just the current summary row. Reads `ColdStore`'s uncapped history via
  new `Manager::export_target_full_csv()`/`export_all_targets_full_csv()`.
- **Fixed along the way:** these CSV exports used JSON-string escaping
  (`esc()`) instead of proper RFC 4180 CSV escaping — a target name or
  hostname containing a comma would silently shift every column after it.
  Added a dedicated `csv_esc()` in `manager.hpp`.
- **Excel export** (per target and fleet-wide), via SheetJS (`xlsx`) — see
  the dependency note below for why it's installed from SheetJS's own CDN.
  Per-target: Config + Hop Summary + Full History as separate sheets in one
  workbook. Fleet-wide: Targets Overview + All Hops Summary + Full History
  across every target.
- **Traceroute PNG** — a picture of the hop table itself (distinct from the
  existing latency-graph PNG export), via
  [html-to-image](https://github.com/bubkoo/html-to-image). Deliberately
  *not* the far more commonly reached-for `html2canvas`: it has a long-
  standing, unresolved bug failing to parse `oklch()`/`color-mix()`
  ([niklasvh/html2canvas#3269](https://github.com/niklasvh/html2canvas/issues/3269)),
  and this app's Tailwind v4 theme uses `color-mix()` throughout. html-to-
  image sidesteps this by rendering through an SVG `foreignObject` — the
  browser's own CSS engine paints it, not a reimplementation of CSS color
  parsing.

### Fleet-wide Excel export: fixed a real UI freeze, then rebuilt for scale

The fleet-wide Excel export button froze the entire app for several seconds
before the save dialog appeared. Root-caused in two layers, both fixed:
1. Building the workbook (CSV parsing + `aoa_to_sheet` + `write`) ran
   synchronously on the main thread — moved to a Web Worker
   (`tauri-app/src/workers/xlsxWorker.js`).
2. That alone wasn't sufficient: the fleet's full history was fetched as
   *one* backend call returning *one* potentially huge string, and handing
   that string to the worker via `postMessage` structured-clones it — a
   synchronous main-thread copy regardless of where the parsing happens.
   **Redesigned** to reuse the existing, already-bounded per-target
   `exportTargetCsv(id)` command in a sequential loop (one target at a
   time), streaming each chunk into a persistent worker as a *transferred*
   `ArrayBuffer` (zero-copy) via a `start`/`append`/`finish` protocol. Every
   backend call, IPC decode, and transfer is now bounded by one target's
   data, never the whole fleet's, and each `await` in the loop yields back
   to the browser between targets — real per-target progress
   ("Exporting 47/130…", shown on the button itself) falls out of this for
   free. Sequential rather than parallel on purpose, to avoid contending
   `Manager`'s shared locks with concurrent requests at real fleet scale.

### Dependencies

- Added [`motion`](https://motion.dev/) (MIT) — drag-to-reorder.
- Added [`html-to-image`](https://github.com/bubkoo/html-to-image) (MIT) —
  Traceroute PNG export.
- **`xlsx` (SheetJS, Apache-2.0) is installed from SheetJS's own CDN
  (`cdn.sheetjs.com`), not the npm registry.** The npm-published build is
  permanently frozen on an old release with two known high-severity
  vulnerabilities — prototype pollution
  ([GHSA-4r6h-8v6p-xvw6](https://github.com/advisories/GHSA-4r6h-8v6p-xvw6))
  and ReDoS
  ([GHSA-5pgg-2g8v-p4x9](https://github.com/advisories/GHSA-5pgg-2g8v-p4x9))
  — because SheetJS stopped publishing fixed releases to npm after a policy
  dispute; the CDN is their own documented distribution channel for current
  versions. This requires `tauri-app/.npmrc`'s `allow-remote=all`: recent npm
  versions default to refusing any dependency specified as a raw tarball
  URL (a good default in general, and exactly what this one dependency
  intentionally does). Two practical consequences documented in `.npmrc`'s
  own comment and `README.md`: `npm install`/`npm ci` needs outbound access
  to `cdn.sheetjs.com`, not just the npm registry; and `.npmrc` must actually
  be committed for a fresh clone (including CI) to have it.

## 0.9.5 - Real-machine compile fixes: missing deps, IPC permissions, ping pipeline, corrected firewall rules

Everything in this release surfaced only once the app was actually built and run
on real Windows hardware for the first time — several of these are pre-existing
gaps that had no way to be caught earlier without a working Rust/Windows
toolchain to compile against.

- **Fixed: two plugins referenced in code but never actually declared as
  dependencies.** `tauri_plugin_dialog::init()` was called in `lib.rs` with no
  `tauri-plugin-dialog` entry in `Cargo.toml`; `@tauri-apps/plugin-dialog` was
  imported in `tauri-bridge.js` with no matching `package.json` entry. Both
  failed the build/dev-server outright with clear errors once actually compiled.
- **Fixed: Ping tab produced no output at all**, from two independent bugs that
  both hid behind "the process runs fine in the background, the UI just never
  updates":
  - `core:event:allow-listen` was missing from `capabilities/default.json`,
    so `listen('np-ping-line', ...)` rejected with a permission error — an
    unhandled promise rejection with no user-visible feedback.
  - `ping_start` returned a bare ID string instead of `{ id, cmd }`; the
    frontend's `res.id` access on a plain string silently evaluated to
    `undefined`, so the `id === idRef.current` correlation check between
    streamed events and the active ping session never matched, ever.
  - Also fixed: streamed ping lines rendered as one run-on block with no line
    breaks — sibling `<span>` elements with nothing between them don't get
    separated by `<pre>`'s whitespace preservation, since there was no
    whitespace there to preserve. And: clicking Ping with an empty host field
    did nothing, with zero feedback.
- **Fixed: Cargo workspace mis-resolution.** `cargo` searches parent
  directories for a workspace root when none is declared in the current
  package, so any unrelated `Cargo.toml` sitting in a parent folder (e.g.
  wherever this repo happens to be extracted to) was silently mistaken for
  this project's own workspace, producing a confusing "can't find library
  netpulse_lib" error. Fixed with an explicit empty `[workspace]` table.
- **Fixed: window un-maximize/resize corruption on every Windows focus-regain.**
  The WebView2 stale-frame repaint workaround (added this cycle — see 0.9.4)
  called `set_size()` unconditionally on `Focused(true)`. `inner_size()` while
  a window is maximized returns the maximized (near-fullscreen) dimensions,
  not a separate "restore" size — Tauri's window API has no way to read the
  true pre-maximize size at that point. Reapplying that fullscreen size via
  `set_size()` (which itself un-maximizes) permanently corrupted the window's
  remembered restore size: every later un-maximize, even a manual one, snapped
  to fill the screen, and since this fired on every focus-regain it repeated,
  visibly "popping" between states on every alt-tab. Fixed by skipping the
  nudge entirely while maximized.
- **Fixed: Windows Firewall NSIS hooks allowed the wrong ICMP traffic.** The
  original rules used `icmpv4:8,any` / `icmpv6:128,any` (Echo Request only)
  for *both* directions. That's correct outbound (sending our own probes) but
  wrong inbound — it only let *other hosts* ping `netpulse.exe`, not the
  actual Echo Reply / Time Exceeded / Destination Unreachable traffic
  traceroute needs back. Windows Firewall's normal "allow replies to my own
  outbound request" connection tracking doesn't help here either, since
  traceroute's replies come from a *different* host at every hop, not the
  final destination the packet was addressed to — which is exactly why an
  explicit, type-unrestricted inbound rule is required at all. Corrected to
  unrestricted `protocol=icmpv4`/`icmpv6` in both directions, still scoped to
  `netpulse.exe` specifically via `program=`, not a system-wide allow.
- **New: Linux raw-ICMP permission automated for real, not just documented.**
  A `.deb` postinst script (`setcap cap_net_raw+ep` on the installed binary)
  is spliced into the already-built `.deb` as a release-workflow post-build
  step, since Tauri's bundler has no config surface for maintainer scripts at
  all (confirmed against the open upstream issue tauri-apps/tauri#8993).
  Verified against a real mock `.deb` built and read back by `dpkg-deb`
  itself, not just written blind — same one-time-at-install-elevation pattern
  as the Windows NSIS hooks, no `sudo`/root needed at every launch. AppImage
  and macOS still need a manual one-time step: AppImage has no install
  step to hook into at all, and macOS's real fix needs a paid Apple Developer
  account for a signed privileged helper, which the project doesn't have yet.
- **New: "Force recheck" restored to the Tauri app.** Fully wired at the
  Rust/bridge layer (`force_recheck` command, `forceRecheck` bridge function,
  capability grant) but had no UI trigger anywhere in `tauri-app/src/App.jsx`
  — apparently never ported over from the sibling `web/src/App.jsx` during the
  original Tauri migration. Menu item, per-target sidebar button, and main
  detail-panel button all restored, matching the web app's implementation
  exactly (non-destructive on-demand route re-verification, with a brief
  local "recheck requested…" pulse for immediate feedback since the engine
  doesn't reset any state for `isDiscovering()` to react to).
- **Fixed: DNS lookups silently failed on restrictive networks.**
  `hickory-resolver`'s `ResolverConfig::default()` hardcodes Google's public
  DNS servers (8.8.8.8/8.8.4.4) rather than reading the system's actual
  configured resolver — on a network that only permits the system-configured
  DNS server (the same class of restrictive setup that blocked ICMP on some
  test machines), direct queries to those hardcoded external servers were
  silently blocked while `nslookup`/browsers worked fine via the properly
  allowed system resolver. Switched to `Resolver::builder_tokio()`.
- **Fixed: sidebar target card's secondary line (IP/type/probe-interval) sat
  visibly out of alignment with the status lamps above it**, in both compact
  and dashboard layouts. Root cause was structural, not cosmetic: `.tc-dest`
  was nested inside `.tc-id`, stacked directly under `.tc-name` — so its
  left edge inherited wherever the bold, proportional-font name text
  happened to start, while `.tc-name` and `.tc-dest` render in different
  fonts/sizes with different left side-bearing, and neither position had
  any relationship to where `.signal` (the lamps) actually sits. Moved
  `.tc-dest`/`.tc-error` out of `.tc-id` into a new sibling `.tc-subrow`,
  indented by `calc(20px + 10px)` — `.drag-handle`'s fixed width plus
  `.tc-row1`'s flex gap, both constants — so it lines up under the lamps
  in every mode regardless of font metrics or selection state.
- **Fixed: `.tc-dest` rendered at full `--text` brightness instead of
  muted.** Its className included a literal `muted` class, but no CSS rule
  for a bare `.muted` exists outside `.drawer`/`.update-banner` scopes, so
  it silently did nothing. Given an explicit `color: var(--muted)`.
  Also dropped `font-mono` from this line and a duplicated space+margin gap
  before the family label, to match the plain, non-monospace `text-faint
  text-[11px]` styling the original (pre-Tauri) sidebar used for the same
  line.
- **Fixed: selected-card accent bar sat flush against the card edge with no
  breathing room, and read as too thin.** `left`/`width` were hardcoded to
  5px/3px; both are now `--tc-accent-bar-left`/`--tc-accent-bar-width`
  (8px/5px), and selected compact cards get `--tc-sel-extra-indent` (4px)
  of extra left padding beyond the shared 18px gutter so the bar has
  visible space before the drag handle.
- **Fixed: card-reorder drag animation visually corrupted uninvolved rows
  when 3+ targets were present.** `useFlipAnimation`'s FLIP hook called
  `Element.animate()` on every reordered row without ever canceling a
  still-running animation from a previous swap. A fast drag across several
  cards can trigger a second swap on the same row before its first 260ms
  animation finishes; the Web Animations API doesn't blend two independent
  `transform` animations on one element sanely, so the row visibly snapped
  toward whichever animation was winning that frame — which looked like
  unrelated cards jumping to the opposite side of the one actually being
  dragged. Fixed by tracking the in-flight `Animation` per row and calling
  `.cancel()` on it before starting a new one.
- **Fixed: "Save list (.npulse)" / "Export JSON" wrote targets in raw
  engine order, not the user's manually drag-reordered arrangement** — the
  export payload mapped over `targets` directly instead of sorting by
  `customOrder`. Now sorted via the existing `orderIndex()` helper before
  export. "Load list" previously re-added targets in file order but never
  told `customOrder` about the new ids, so even a correctly-ordered file
  imported into whatever order the engine happened to report the new
  targets back in; import now captures each newly-created id in the file's
  own order and folds it into `customOrder` afterward.
- **New: dashboard toolbar actions.** Pause/Resume all, Save list, Export
  JSON, and Load list previously only existed in the sidebar's stacked
  button column (`compact` mode); the dashboard view had no equivalent.
  Added as a row of pills at the end of the dashboard toolbar, reusing the
  same handlers — the sidebar's stacked full-width layout doesn't suit the
  dashboard's horizontal space, so this is a new `.dashboard-actions`
  layout, not a copy-paste of the sidebar markup.
- **Improved: dashboard card visual pass.** Non-compact target cards now
  get a status-colored left accent bar (ok/warn/bad/down/discovering,
  reusing the same state colors as the status lamps) for at-a-glance
  scanning of a long list; the Latency metric tints to match the card's
  overall status; the status label upgraded from plain colored text to a
  filled pill; and card padding opened up slightly (`13px 16px 13px 18px`)
  so the row reads less cramped.
- **New: About box shows the real running version and a link to the
  GitHub repo.** Previously just a title and one-line description with no
  version number anywhere in the UI. Version is read via
  `@tauri-apps/api/app`'s `getVersion()` (a new minimal `core:app:
  allow-version` grant in `capabilities/default.json`) rather than a
  hardcoded string, so it can't drift out of sync with the actual build at
  the next release.

## 0.9.4 - Tauri hardening: crash fixes, install-time permissions, auto-update

- **Fixed: production-only crash on first chart render**
  (`Cannot read properties of undefined (reading 'has')`, every installed
  build, every machine, `tauri dev` unaffected). Root cause: the JS bundle
  obfuscator was running over third-party library code (React, ReactDOM,
  Recharts, D3, `@tauri-apps/api`) along with the app's own source, since
  Vite bundles everything into one chunk by default. Control-flow flattening
  and identifier renaming were never designed to survive being applied to
  vendor code, and were confirmed to corrupt internal Map/Set-based library
  logic. Fixed via `autoExcludeNodeModules: true`, scoping obfuscation to the
  app's own `src/` files only — vendor code ships unobfuscated (no real loss,
  it's open source already), app code stays fully obfuscated.
- **Fixed: CSP blocked a legitimate dependency internal.** `script-src 'self'`
  (no `unsafe-eval`) was blocking a `Function('return this')()` UMD
  global-object-detection fallback baked into `decimal.js` (a transitive
  dependency pulled in via Recharts) — inherent to that library's own bundled
  source, not something the obfuscator introduced. Added `unsafe-eval`; safe
  here since `script-src` stays `'self'`-only regardless (no remote script
  loading possible either way).
- **Fixed: traceroute permanently capped at the shortest path ever seen after
  a route change** (VPN toggle, network switch, etc.) — `dest_hop_` tracked
  the *minimum* hop the destination was ever reached at with no decay
  mechanism, unlike the sibling `max_hop_seen_`, which already had one (with
  a comment describing this exact failure mode almost verbatim). Once pinned,
  the engine stopped probing past that hop forever, so a shorter VPN-era path
  permanently prevented rediscovering the real, longer path afterward. Added
  matching staleness-based decay, reusing the existing per-hop reply
  timestamps.
- **Fixed: false-positive "routing loop" warning for two adjacent hops sharing
  one address** — confirmed benign against Windows' own `tracert` (common for
  MPLS/tunnel hops or load-balanced routers). A genuine loop requires packets
  to actually cycle, which needs at least a 2-hop gap to be structurally
  possible; now requires that gap before considering it loop evidence at all.
- **Fixed: a reply from a private/CGNAT address could be misattributed as
  reaching a public destination**, permanently pinning discovery at hop 1
  (the LAN gateway) — raw ICMP sockets receive all ICMP traffic on the
  interface, not just replies to this app's own probes, so a coincidental
  id/sequence collision with unrelated traffic could get misattributed to the
  pending-probe table. Added a defensive invariant rejecting this regardless
  of the underlying mechanism: a private address can never legitimately mean
  a public target was reached, full stop.
- **New: Windows Firewall exceptions added automatically via NSIS installer
  hooks**, scoped to `netpulse.exe` specifically — no more silently-blocked
  ICMP on machines with a restrictive default firewall profile (confirmed on
  at least two test PCs), and no UAC prompt needed at every app launch, since
  the installer is already elevated to write to Program Files.
- **New: auto-update via `tauri-plugin-updater`**, checking GitHub Releases
  directly with no separate hosting infrastructure needed. The update
  manifest (`latest.json`) is generated by a custom release-workflow script
  rather than `tauri-action`'s own built-in generator, to avoid a real
  architectural conflict: that generator needs the action to manage the
  GitHub Release itself (its own token, its own tag/release creation), which
  collides with this project's deliberate build-only-then-separately-publish
  pipeline (see 0.9.1's Electron-era equivalent fix for the same class of
  problem).
- **Raw ICMP toggle removed from the UI.** Confirmed Windows has no
  unprivileged ICMP socket path at all — `SOCK_DGRAM + IPPROTO_ICMP` isn't
  supported by Winsock, unlike Linux's `ping_group_range` mechanism — so the
  toggle was either a no-op or actively misleading depending on state. Raw
  stays on unconditionally, on every platform.

## 0.9.2 - Pause/resume sync fix + free code-signing guide

- **Fixed: hop-level pause buttons out of sync with target-level pause.** The hop-pause
  button (tauri-app/src/App.jsx) only checked its own entry in `pausedHops`, with no
  awareness of the target's own `paused` state — clicking it while the whole target was
  paused silently mutated `pausedHops` but had no visible effect (nothing was being probed
  either way), and the icon never reflected reality. Confirmed the backend (Manager::pause
  for the whole-target atomic, Settings.paused_hops for per-hop skip) was already correct
  and independent by design — this was specifically a frontend display/interaction bug.
  Fixed: hop rows now compute an effective paused state (target-paused OR hop-paused), the
  button is disabled (with an explanatory tooltip) while the target is paused, and every row
  visually reflects a target-wide pause, not just individually-paused hops.
- **New: free code-signing section in CODE_SIGNING.md.** SignPath Foundation (Sectigo-backed,
  genuinely free for qualifying OSS, real SmartScreen trust) as the primary path — including
  the eligibility detail that a project needs an existing public release first, and the
  publisher-name tradeoff ("SignPath Foundation", not your name). Also covers Azure Trusted
  Signing's Feb 2026 US/Canada/EU/UK-only public-trust restriction, macOS's real $99/year
  floor (no free path exists), and free Linux options (GPG, Sigstore/cosign).


## 0.9.1 — Tauri + Rust migration (NAPI discarded)

- **New primary app: `tauri-app/`** — Tauri 2 + React 19 + Rust, replacing Electron +
  Node-API. The C++ engine (`core/`) is UNCHANGED; a new flat adapter
  (`tauri-app/src-tauri/native/netpulse_ffi.{hpp,cpp}`) exposes it to Rust via `cxx`
  (primitives + JSON strings, mirroring napi.cpp's validation/clamping exactly).
- **Verified end-to-end** (not just written): Rust calling the real engine through cxx
  (add_target -> real probe thread -> get_state_json -> real hop data -> remove_target);
  the new Rust DNS/reverse-DNS (hickory-resolver) and TCP port-scan (tokio) against real
  network I/O; the ping spawn/stream/stop lifecycle (caught and fixed a real Linux stdout-
  buffering bug in the process — see docs/TAURI_MIGRATION.md). Two real design bugs
  (a ping-event-id capture bug, and an update_target partial-merge bug that would have
  silently reset unspecified fields to defaults) were caught during development and fixed
  before shipping, not left as known issues.
- **IPC protection**: Tauri's capability/permission system
  (`capabilities/default.json` + `AppManifest` in build.rs) explicitly allowlists every
  command the frontend can call — direct architectural equivalent of the old contextBridge
  allowlist, enforced by the framework.
- **Honest limitation**: the full Tauri app (lib.rs/commands.rs/tauri.conf.json/
  capabilities) could NOT be compiled in the environment this was built in — the only
  available Rust toolchain (apt's rustc 1.75) is too old for current Tauri (needs ~1.90+),
  and rustup's download domain wasn't reachable from that sandbox. Every API used was
  cross-checked against real source fetched from tauri-apps/tauri's repository (not
  memory), but "grounded in real source" and "compiled" are different claims — see
  docs/TAURI_MIGRATION.md for exactly which pieces are which, and what to check first if
  the initial build hits an error.
- **electron/ and napi/ retired and deleted.** The Tauri build was verified end-to-end
  on a real machine first (toolchains installed for real, `cargo build --release`,
  `cargo test`, and `npx tauri build` all passing, producing a working installer with
  no `.node`/`.so`/`.dylib` addon) — only then were the old Electron/Node-API app and
  the superseded `ci.yml`/`release.yml` workflows removed.
- **Obfuscation mechanism replaced, verified end-to-end**: the old `NETPULSE_OBFUSCATE`
  switch (both root `CMakeLists.txt` and `napi/CMakeLists.txt`) required an
  obfuscating Clang fork (Hikari/obfuscator-llvm) that was never actually built or
  tested. This session built a real out-of-tree LLVM pass-plugin against
  conda-forge's `llvmdev` and confirmed it compiles/links cleanly, but every
  prebuilt clang.exe available (winget's `LLVM.LLVM`, VS 2026's bundled Clang)
  crashes with `STATUS_HEAP_CORRUPTION` loading it — a confirmed ABI mismatch
  between separately-built LLVM copies, not fixable short of building LLVM from
  source. Replaced with `core/include/netpulse/obfuscate.hpp`: compile-time string
  encryption + opaque-predicate branches, pure C++20, no special compiler required.
  `NETPULSE_OBFUSCATE`/`obfuscate` now just define a preprocessor macro on any
  toolchain — see docs/OBFUSCATED_BUILD.md for the full writeup.


## 0.8.1 — dual build pipeline (normal + LLVM-obfuscated), verified

- **New `NETPULSE_OBFUSCATE` CMake option** (root `CMakeLists.txt` and `napi/CMakeLists.txt`,
  default OFF): a second build variant for the engine using LLVM control-flow obfuscation
  passes, alongside the normal build — same source, two configurations. Requires an actual
  obfuscating LLVM/Clang toolchain (Hikari or obfuscator-llvm; mainline clang does not have
  these passes) and **fails the build loudly** if one isn't in use, rather than silently
  shipping a non-obfuscated binary while claiming otherwise — verified against real clang-18.
- **New `.github/workflows/obfuscated-build.yml`**: builds and caches an obfuscating LLVM
  toolchain (Hikari, from source — no prebuilt releases exist) and runs the exact same
  `tests/test_core.cpp` suite against the obfuscated binary. Kept as its own workflow (not a
  job in `ci.yml`) since a cold LLVM build is a multi-hour undertaking; cached after the first
  run. Triggers: manual, weekly, or on CMake/workflow changes — not every push.
- **New `docs/OBFUSCATED_BUILD.md`**: honest writeup of what was verified (the gating logic,
  the normal build) vs. what requires a real toolchain build I couldn't do in this session
  (the actual obfuscated compile), plus the Windows/MSVC caveat (Hikari/obfuscator-llvm are
  Clang-based; `cl.exe` can't use them — `clang-cl` or cross-compilation needed).
- Confirmed normal build + full test suite pass unaffected, with both GCC and Clang,
  Release and Debug, from the exact packaged tree.




## 0.8.1 — global socket pool, checksum-pinned loop auditor, edge-keyed cache

- **Socket pool + centralized RX dispatch (root-causes the last of the
  shared-hop loss, at any target count).** Every session used to own an
  exclusive raw socket; on Windows an inbound ICMP reply is not guaranteed
  to be delivered to every raw socket that could accept it, so a reply for
  target B could still land on target A's socket and be silently dropped
  even after 0.7.6's reply-routing fix, if it never reached a socket that
  knew to route it. Fixed at the root: sessions no longer own sockets.
  `acquire_pooled_socket(family, privileged, source_addr)` hands out one
  shared, ref-counted socket per distinct combination (typically 1-2 for the
  whole process; a genuinely multi-homed/VPN setup still gets its own socket
  per distinct interface, unchanged from before). A single background
  `RxDispatcher` thread `select()`s across every pooled socket and is now
  the *only* code that ever calls `recvfrom()`; every parsed reply is looked
  up by ICMP id in the existing global registry and pushed straight into
  the owning session's inbox. Verified: 1 target and 100 targets both hold
  ~0%% loss on shared early hops (router, ISP BNG) with no dependency on
  which socket the OS happened to deliver a reply to.
- **Fixed a real TX race exposed by socket sharing.** `IP_TTL` /
  `IPV6_UNICAST_HOPS` is socket-level state set via `setsockopt()`
  immediately before `sendto()`, not a per-packet parameter — with multiple
  target threads now sending on the same pooled socket, two concurrent sends
  could interleave `{set TTL A} {set TTL B} {send, now carrying the wrong
  TTL}` and silently corrupt a probe's hop count. Fixed with a per-socket
  mutex wrapping exactly that `{setsockopt, sendto}` pair.
- **Paris-traceroute checksum pinning (removes a false-positive source at
  the root, not just papering over it with a heuristic).** Investigated why
  the same public IP could legitimately appear at two different hop depths
  with no real routing loop: routers load-balancing across equal-cost links
  (ECMP) often hash on the ICMP checksum to pick a branch, and this engine's
  probes to different TTLs carry different `seq` (hence different
  checksums), so they could genuinely take different physical paths and
  land on the same shared router at two different hop counts. `build_echo()`
  now optionally pins every IPv4 probe in a session to the same fixed
  checksum (solved algebraically via RFC 1071 one's-complement arithmetic,
  written into 2 reserved payload bytes), so ECMP always routes every probe
  in a session down the same path — this class of false "loop" simply can't
  happen anymore on IPv4. IPv6 doesn't need it: RFC 6438 mandates IPv6 ECMP
  hash the Flow Label instead of the ICMP payload.
- **Topological loop auditor, backoff instead of a hard stop.** A dead or
  flapping WAN link can make one local device answer discovery probes at
  many TTLs at once (the packet bounces between a couple of real routers
  until it expires locally); left unhandled, every one of those hops starts
  its own full-rate direct-echo stream to the same 1-2 devices — a "ghost
  train." The engine now scans for a lower-hop address duplicate on every
  genuine reply, requires **two independent** confirmations before treating
  it as a real loop (a transient ECMP coincidence won't repeat; a real loop
  will, every time — this is the backstop for whatever variance checksum
  pinning doesn't remove), and, once confirmed, narrows the fast discovery
  window to the loop boundary +1 hop instead of sliding it out to
  `max_hops`. This is a deliberate **backoff, not a stop**: the boundary hop
  keeps getting one real, slow (~8s) legacy probe forever, so a route change
  or link recovery is always noticed and the window re-opens automatically
  the moment a clean reply arrives — never a silent freeze.
- **Edge-attributed shared-hop cache key.** The cross-target cache (0.8.0)
  keyed only on `(source, responder IP)`, which conflated "the same router,
  reached the same way" (safe to share) with "the same public IP visible
  from two genuinely different upstream paths" (should NOT silently
  overwrite each other's real numbers). The key is now
  `(source, predecessor hop's address, responder IP)` — targets sharing the
  same path prefix still collapse onto one entry (the common, valuable
  case), while targets reaching the same node via a different predecessor
  now get independent measurements. `predecessor_of()` walks backward
  through however many lower hops are unresolved to find the *nearest
  actually-resolved* hop, rather than only checking hop-1 directly — many
  real routers silently forward without ever answering a TTL-limited probe,
  so a silent hop-1 does not mean the whole predecessor chain is unknown,
  and treating it as unknown would incorrectly split one stable route's
  cache entry in two. `SharedHopTable`'s lock is now a `std::shared_mutex`
  (concurrent adopters never block each other on a read-mostly table), and
  entries are attributed by `Session::id_` (a stable, never-reused counter)
  instead of a raw, address-reuse-fragile `const void*`.
- **Fixed `npm run dist`.** electron-builder 26.x's schema requires
  `publisherName` nested under `win.signtoolOptions`, not directly under
  `win:` — an older layout that failed schema validation and blocked every
  signed installer build. Corrected in `electron/electron-builder.yml`;
  verified end-to-end (signed NSIS installer + blockmap produced).
- **Added `ARCHITECTURE.md`, `PRIVACY_POLICY.md`, `CODE_SIGNING.md`, and
  `.github/FUNDING.yml`**, plus a GitHub Pages project site under `docs/`.
  `ARCHITECTURE.md` documents the full engine design (threading model,
  socket pool, direct-echo model, loop auditor, shared-hop cache, DNS pool)
  for anyone modifying `core/`.

## 0.8.0 — production hardening (real, verified) + security-model writeup

- **Release native builds now strip symbols** and hide non-required exports
  (`-fvisibility=hidden` + linker `-s` on Unix; `/OPT:REF /OPT:ICF` + no `.pdb` on MSVC).
  Verified: unstripped ~3,100 symbols / ~1.1 MB -> stripped 0 local symbols / ~380 KB.
- **DevTools + the default Electron menu are disabled in packaged builds** (F12,
  Ctrl/Cmd+Shift+I blocked; DevTools force-closed if opened programmatically). DEV builds
  keep them for iteration.
- **New SECURITY.md section: "Reverse engineering / IP protection — what's actually
  achievable."** Honest technical writeup: no client-side technique (obfuscation, static
  linking into another language, a different app shell) can keep logic secret from someone
  who controls the device running it; obfuscation raises attacker cost, it doesn't remove
  the possibility. Also notes the practical tension with this project's own AGPL-3.0-or-later
  choice: recipients of a binary are legally entitled to its corresponding source, and that
  source is already public, so obfuscating the binary doesn't hide anything from someone
  motivated enough to read the repo instead.

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
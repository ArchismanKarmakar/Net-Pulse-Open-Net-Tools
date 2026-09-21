# Changelog

## 1.2.4

### Fixed: CI (`cargo check --features obfuscate`) failed on Windows, Linux AND macOS with a CMake "CMakeCache.txt directory ... is different" error

**Bug report** (CI logs, all three OS runners): every job failed inside
`build.rs`'s CLI-sidecar step with `CMake Error: The current
CMakeCache.txt directory .../build-cli-sidecar/CMakeCache.txt is
different than the directory c:/Users/Archisman/Downloads/NetPulse-cpp-
web/33/NetPulse-cpp-web/build-cli-sidecar where CMakeCache.txt was
created`, then a panic ("Could not automatically build the CLI sidecar").

**Root cause**: `build-cli-sidecar/` wasn't covered by `.gitignore` (only
the exact name `build/` was listed — `build-cli-sidecar` doesn't match
that), so a `CMakeCache.txt` generated on a local machine — which bakes
in the exact absolute source path it was configured from — got committed.
Every fresh checkout, on every OS (all three jobs' error output shows the
SAME committer's local Windows path, confirming it's one committed file
being cloned everywhere), inherited a cache that could never match ITS
OWN checkout path. CMake correctly refuses to silently reuse a
cache recorded against a different location, so the build hard-failed
before compiling anything.

**Fix, two parts**:
1. `.gitignore`: `build/` widened to `build*/` (directories only — can't
   match the `build-and-run.sh`/`.ps1` *files* at the repo root) so this
   whole family of local CMake scratch directories (`build-cli-sidecar/`,
   `build-cli/`, `build-verify/`, `build-asan/`, and any future one) is
   covered generically instead of needing a new line every time.
2. `build.rs` (`ensure_cli_sidecar`): now checks the existing cache's own
   recorded `CMAKE_HOME_DIRECTORY` against the current repo root before
   reusing it, and if they don't match, deletes the whole
   `build-cli-sidecar/` directory and reconfigures from scratch instead of
   handing CMake a cache it will just refuse and panic on. Makes the build
   self-healing regardless of how a mismatched cache gets there again (a
   moved checkout, a CI cache-restore action, ...), not just for this one
   already-committed file.

**Action still needed** (can't be done from a code patch alone): the
already-committed `build-cli-sidecar/CMakeCache.txt` needs removing from
version control in the real repository —
`git rm -r --cached build-cli-sidecar` (and any other stray `build*`
directory `git status` shows as tracked), then commit. The `build.rs`
self-heal above means CI will now recover even before that cleanup
lands, but the stale file should still come out of history.

**Verified**: reproduced the exact failure locally (planted a
`CMakeCache.txt` with a bogus `CMAKE_HOME_DIRECTORY`, deleted the sidecar
binary to force a rebuild) — confirmed it previously panicked, and with
this fix `cargo build` completes cleanly and produces a real, working
sidecar binary instead.

### Fixed: Settings tab was also boxed into a fixed ~640px column, same as the Interfaces table

**Follow-up** to the Interfaces fix directly below — the same complaint
applied to Settings too: `SettingsPage.jsx` used the generic `"toolpage"`
class (a fixed `max-width: 900px`) plus its own inline `maxWidth: 640` on
top of that, so on any window wider than ~700px the page sat in a fixed
box with the rest of the window left empty next to it.

**Fix**: `SettingsPage.jsx` now renders `<div className="toolpage
fluid">` with no width cap, and the "Auto-refresh" / "New target
defaults" field grids switched from a fixed 2-column layout to
`grid-template-columns: repeat(auto-fit, minmax(200px, 260px))` — more
fields per row as the window gets wider (up to 6 on a large window, down
to 1 on a narrow one), so the extra space is actually used instead of
sitting empty. Verified in a real headless run: 6 fields per row at
1900px wide, 4 at the app's configured minimum (1280px), no dead space
either way, vertical scrolling from the previous fix still intact.

### Fixed: Interfaces table was boxed into a fixed ~900px column with a horizontal scrollbar, leaving the rest of a wide window empty

**Bug report** (with screenshot): on a wide window, the Interfaces table
sat in a narrow fixed-width box requiring its own sideways scroll to see
every column, while the remaining ~half of the window next to it was
just empty background — the page wasn't using the space the window
actually gave it.

**Root cause**: `InterfacesPage.jsx` used the same generic `"toolpage"`
class as every prose/form tool page (Settings, DNS, Ping), which carries
a fixed `max-width: 900px` — a sensible cap for a page of text or a form
(nobody wants a paragraph stretched across a 1900px window), but wrong
for a data table, which should use whatever width the window actually
has. On top of that, the previous round's fix for the table's own
overflow had added an explicit `min-width: 660px` to `.iface-table` —
redundant on top of `white-space: nowrap` (which already sets a real,
content-driven floor) — that made the table need its own horizontal
scroll even more readily than the content required.

**Fix**: `InterfacesPage.jsx` now renders `<div className="toolpage
fluid">` — a new `.toolpage.fluid` modifier (`styles.css`) that drops the
`max-width` entirely for this page only (Settings/DNS/Ping keep their
existing reading-width caps, which are correct for forms). Removed the
redundant `min-width: 660px` on `.iface-table`. Verified in a real
headless run at both a wide window (1900×1000 — table now spans the full
width, no dead space, no scrollbar) and the app's configured minimum
(1280×720 — table still fits cleanly with room to spare).

### Fixed: Interfaces and Settings tabs had no scrollbar on a small window — content (MTU/Egress columns, the Appearance section, Save button) was clipped, not missing

**Bug report**: on a small window, the bottom of the Settings tab (the
Appearance section and the Save/Reset buttons) and the right side of the
Interfaces table (the MTU and Egress columns specifically) were simply
unreachable — no scrollbar, nothing to scroll, in either direction.

**Root cause, vertical** (`styles.css`'s `.toolpage`): every tool page
(Interfaces, Settings, DNS, Port Scanner, Ping) renders as a direct sibling
of `<nav class="tabbar">` inside `.app`, and `.app` is a fixed-height flex
column (`flex flex-col h-screen overflow-hidden`). `.toolpage` itself had
no `flex`/`overflow` rules at all, so on a window short enough that a
page's content didn't fit, the parent's `overflow: hidden` just clipped
everything past the bottom edge — there was nothing there to scroll.

**Root cause, horizontal** (`.iface-table-wrap`): this card used
`overflow: hidden` to keep its rounded corners. On a narrow window, that
didn't just clip the corners — it clipped the whole right side of the
table along with them, MTU and Egress columns included. They weren't
missing from the data or the markup; they were rendered and then silently
cut off with no scrollbar to reach them.

**Fix**: `.toolpage` now gets `flex: 1 1 auto; min-height: 0; overflow-y:
auto` — `min-height: 0` matters as much as the other two, since a flex
item's default `min-height: auto` means "never shrink below my content's
height," which would otherwise defeat `overflow-y: auto` entirely.
`.iface-table-wrap` switched to `overflow-x: auto`, and `.iface-table`
gained a `min-width` (plus `white-space: nowrap` on cells) so it actually
overflows its wrapper on a narrow window and produces a real horizontal
scrollbar, instead of just shrinking every column illegibly and never
triggering one. Verified in a real headless run at the window's configured
minimum size (1280×720): the Settings tab's scrollbar appears and reaches
the Appearance section and Save button; the Interfaces table's MTU/Egress
columns render correctly with room to spare (this build's added "Kind"
column, below, would have made this worse without the fix).

### Fixed: TCP/UDP hops that were never actually probed yet showed a green "healthy" dot, identical to a real reply

**Bug report** (with screenshots): a TCP:443 trace to 1.1.1.1 showed hops
4–9 with `SENT=0, RECV=0, LOSS=0.0%` — i.e. no probe had ever been sent to
those hops at all — yet each one's status dot was green, indistinguishable
from a hop that had actually replied. The equivalent ICMP trace correctly
showed grey/hollow dots for hops that were genuinely asked and got nothing
back.

**Root cause** (`tauri-app/src/App.jsx`, `hopStatus()`/`isDiscovering()`/
`destLamp()`/`pathLamp()`/`targetState()`): every one of these derived a
"no reply" condition as `sent > 0 && recv === 0` — correct for "asked,
got nothing back", but it left a gap for `sent === 0` ("never asked at
all"), which fell through every check to the same `return 'ok'` default
as a real healthy reply. TCP/UDP hit this far more than ICMP: TCP's
hop-discovery only sends a probe for a given TTL once discovery actually
reaches it (see `session.cpp`'s outstanding-probe bookkeeping), so a hop
beyond where discovery currently is — or beyond where the destination was
already confirmed at a lower TTL — sits at `sent=0/recv=0`, sometimes for
a while, sometimes indefinitely. ICMP's discovery sends every TTL up
front, so it rarely shows this gap.

**Fix**: `sent === 0` is now checked explicitly, first, everywhere the old
`noReply` pattern appeared:
- `hopStatus()` returns a new `'pending'` state for an unprobed hop,
  rendered as a **hollow ring** (`.hopdot.st-pending`, `styles.css`) —
  visually distinct from both a real green reply and a grey "silent"
  (asked, no answer) hop, instead of collapsing into the same green as a
  healthy one.
- `isDiscovering()` no longer treats a destination-hop record that exists
  but hasn't been probed yet (`sent === 0`) as "not discovering" — which
  was letting `targetState()`/`destLamp()`/`pathLamp()` fall through their
  own thresholds with all-zero data and land on a false "ok" the same way.

Verified live in this sandbox: a real ICMP trace to 1.1.1.1 produced
exactly this shape mid-discovery (hop 1 real green reply, hop 2 not yet
probed, hop 3 probed/100% loss) and the three hops now render as three
visibly different dots — filled green, hollow ring, filled grey —
matching the fix's intent (see the engineering session's own note; the
same code path drives TCP/UDP identically since `hopStatus()` etc. are
protocol-agnostic).

### Improved: source-Interface dropdown and the Interfaces tab now show each adapter's kind (Wi-Fi / Ethernet / Virtual)

**Follow-up to the report above** ("fix the interface thing... add more
details here"): a generic, driver-assigned adapter name (a Windows
GUID-derived name, or a Linux `enp3s0`/`wlp2s0`) gave no way to tell which
dropdown entry actually WAS the Wi-Fi adapter without separately checking
the OS's own network settings.

**Fix**: `NetInterface` (`core/include/netpulse/transport.hpp`) gained a
best-effort `kind` field — `"Wi-Fi"`, `"Ethernet"`, `"Virtual"` (VPN/
hypervisor/container adapters: vEthernet, VMware, Docker, tun/tap, ...) or
`"Other"`. Windows derives it from the adapter's `IfType`
(`IF_TYPE_IEEE80211` / `IF_TYPE_ETHERNET_CSMACD`); Linux checks for
`/sys/class/net/<name>/wireless` or `/phy80211`, falling back to naming
conventions on other POSIX platforms. Threaded through
`list_interfaces_json`/`list_interfaces_detailed_json` (`netpulse_ffi.cpp`)
and shown:
- in the "Add target" and per-target "Edit config" Interface dropdowns, as
  a `[Wi-Fi]`/`[Ethernet]`/`[Virtual]` prefix on each option;
- as a new "Kind" column on the Interfaces tab (`InterfacesPage.jsx`);
- as a new `KIND` column in the CLI's `npulse ifconfig` (`cli/main.cpp`),
  so all three surfaces agree.

Also added a manual ⟳ refresh button next to the "Add target" Interface
dropdown (calls straight into the existing 20-second poll rather than
waiting on the timer) and a hint line when the list comes back empty,
plus an empty-list check pointing at the Interfaces tab.

### Fixed: "Add target" source-Interface dropdown never picked up an adapter that came up after launch

**Bug report:** an adapter that was up and usable per the Interfaces tab
(Wi-Fi, specifically) never appeared in the main window's "Add target"
source-Interface dropdown.

**Root cause**: the dropdown's interface list (`/api/interfaces` →
`list_interfaces_json`) was fetched exactly ONCE, in a `useEffect` with an
empty dependency array, right when the app mounted (`App.jsx`). If an
adapter connected or reconnected any time after that — Wi-Fi joining a
network a few seconds after launch, a laptop resuming from sleep, a VPN
coming up — it was permanently invisible to that dropdown until the app
was restarted, even though the separate Interfaces tab (which the
connectivity-alert check already polls every 20 seconds) showed it as up
correctly the whole time.

**Fix** (`tauri-app/src/App.jsx`): the dropdown's interface list is now
derived from that SAME existing 20-second poll of `/api/interfaces/detailed`
(filtered to up + non-loopback, exactly matching what
`list_interfaces_json` itself returns), instead of a separate one-shot
fetch — no extra native call added, and the dropdown now genuinely tracks
current adapter state the same way the Interfaces tab does.

### Changed: Settings moved from a separate window into a tab (fixes blank window + app not exiting)

**Bug report:** in real-world testing (not reproducible in this sandbox,
which has no way to launch a real desktop session to click through), the
separate Settings *window* came up blank, and — separately — closing the
main window did not end the app's process while the Settings window was
still open, since Tauri does not tie a secondary window's lifetime to the
main one; with no visible window left, there was nothing left to close it
from short of killing the process externally.

**Fix, per explicit direction ("move it in beside the Interfaces tab in
the same window")**: Settings is no longer a second OS window at all —
it's a new "⚙️ Settings" tab in the main window, right beside "Interfaces".
This removes the whole class of bug rather than patching either symptom:
- It can't come up blank independently of the rest of the app, because
  it's the same page, the same JS context, the same webview as everything
  else — there is nothing separate left to fail to load.
- There is no second window to outlive the first, so closing the main
  window always ends the process, unconditionally.

**Removed**: `open_settings_window` (`commands.rs`, `lib.rs`'s handler
list, `build.rs`'s `COMMANDS`, `capabilities/default.json`'s
`allow-open-settings-window`), `capabilities/settings.json` (deleted — no
second window means no second, narrower IPC surface to scope), and
`SettingsWindow.jsx`/`main.jsx`'s `?window=settings` query-param branch
(also deleted — the whole reason that branch existed).

**Added**: `SettingsPage.jsx` (`tauri-app/src/components/tools/`) — the
same settings UI and load/save logic as before, now rendered as an
ordinary tab (`App.jsx`'s tab bar + `Settings → Open Settings…` menu item
both just call `setTab('settings')`). `tauri-bridge.js`'s
`loadAppSettings`/`saveAppSettings`/`getRecheckTuning`/
`emitSettingsChanged`/`onSettingsChanged` are all unchanged and still used
by the new page exactly as the old window used them — only
`openSettingsWindow` itself is gone, since there's nothing left to open.

**Verified**: a real `cargo build`/`clippy` (clean) and a real production
`vite build` of the frontend (same one-off registry-`xlsx` workaround as
before, for this test only). Launched the actual debug binary headless
under Xvfb serving that real bundle, took a screenshot of the rendered
main window (all five original tabs plus the new "⚙️ Settings" tab
visible in the tab bar), then scripted a click on the Settings tab and
screenshotted again — the full settings form renders (auto-refresh
tuning, new-target defaults, appearance, Save/Reset) with no blank page.
The full C++ suite (untouched — this change is frontend/Rust-only) still
100% passes.

### Improved: Interfaces and Settings tabs now look like part of the app, not pasted-in content

**Report**: both tabs read as bare, borderless content sitting directly on
the window background — no card boundaries, no depth, nothing tying them
visually to the rest of the app's designed dashboard (`.dcard`s, headers,
shadows).

**Fix**: `.toolpage` (shared by every tool page — Interfaces, Settings,
DNS, Port Scanner, Ping) gets a proper header treatment (a bottom border
under the title, a muted subtitle line). New `.tool-section` class gives
Settings' three groups (auto-refresh, new-target defaults, appearance)
real cards — background, border, radius, shadow — instead of plain `<div>`s
with inline margin between headings. The Interfaces table now sits inside
a bordered, shadowed card (`.iface-table-wrap`) with a shaded header row
and zebra striping, instead of floating bare on the window background.
Also added a `.tool-card.success` variant so Settings' "saved" banner gets
the same green-tinted card treatment its "error" banner already had,
instead of losing its color entirely once the layout stopped using inline
styles for it.

**Verified**: a real production `vite build` (same one-off registry-`xlsx`
workaround as the other frontend changes this round) and screenshots of
both tabs from the actual debug binary running headless under Xvfb, before
and after.

### Fixed: opening the Settings window could crash the whole app

**Bug report:** opening the new Settings window (added below) could bring
down the entire application, not just fail to open that one window.

**Root cause:** `open_settings_window` is the *only* command in this
codebase that creates a native OS window/webview on demand from a
`#[tauri::command]` handler — every other command is a pure data
operation. Confirmed by instrumentation that such handlers run on a
background thread, never the main/event-loop thread. Window/webview
creation is native-toolkit work (GTK on Linux, WebView2/COM on Windows,
WKWebView on macOS) with main-thread — or, for WebView2, COM-apartment- —
requirements, and the previous code called
`tauri::WebviewWindowBuilder::build()` straight from that background
thread, trusting Tauri's runtime to dispatch it across threads safely.
That trust turned out to be well-placed on this project's own Linux/GTK
testing (a real debug build, run headless, with the actual production
frontend, deliberately invoked off-thread, did not crash) — but WebView2's
stricter apartment-thread rules on Windows make that implicit dispatch a
narrower guarantee there, and either way, nothing on this path caught a
panic: an unwind escaping through a native callback boundary (a COM
callback, a GTK signal handler) is undefined behavior that typically
surfaces as the *entire process* aborting rather than one command failing
— exactly matching "the whole app crash" instead of just a failed-to-open
Settings window.

**Fix** (`tauri-app/src-tauri/src/commands.rs`, `open_settings_window`):
window/webview construction is now explicitly dispatched onto the main
thread via `AppHandle::run_on_main_thread` (Tauri's own documented
mechanism for this), removing any dependence on the runtime's internal
thread handling, and wrapped in `std::panic::catch_unwind` so any panic
during construction becomes an ordinary `Err(String)` — already shown to
the user via the existing "Could not open Settings" modal in `App.jsx` —
instead of taking the whole process down. The result crosses back to the
calling thread over a one-shot channel with a 10s timeout, so a wedged
main thread can't hang the IPC call forever either.

**Verified:** a real `cargo build` of the fixed binary, run headless under
Xvfb with the actual obfuscated production frontend bundle (built via a
one-off `npm install`/`vite build` for this test only — see the previous
entry's own verification note on why that isn't normally possible in this
sandbox), with `open_settings_window` deliberately invoked from a spawned
background thread (mirroring real IPC dispatch, confirmed via
`std::thread::current().id()`): the window opens, the call returns `Ok`,
and the process stays up. `cargo check`/`cargo build`/`clippy` all clean;
the C++ suite (unaffected — this fix is Rust-only) still passes in full.
This sandbox has no Windows target to compile or run against, so the
Windows/WebView2-specific half of the hypothesis couldn't be reproduced
directly here — the fix is written to hold regardless of which platform's
specifics actually triggered it, since it removes the implicit-dispatch
assumption entirely rather than patching around one platform.

### New: app-wide Settings window, auto-refresh frequency now 30s by default (and configurable)

Two related, explicitly requested changes:

1. **The passive background "auto refresh" mechanism now defaults to 30
   seconds** (lowered from 45s) between legacy-probe rechecks on an
   already-confirmed hop — see the previous entry below for what this
   controls and why it was ~90s+ before anything happened. This was a
   compile-time-only constant; it's now a process-wide **runtime**
   default (`set_default_recheck_tuning()`, `session.hpp`/`session.cpp`),
   clamped to a sane 5–300s / 2–10-misses range, so it can be changed
   without a rebuild.

2. **A new Settings window** (`Settings → Open Settings…`) — a genuinely
   separate OS window, not a panel — lets you change that frequency (and
   its miss-threshold), the defaults a freshly opened "Add target" form
   starts pre-filled with (probe interval, trace interval, timeout,
   payload size, max hops, destination port, family, protocol, raw mode),
   and the dark/light theme. Settings are saved to a JSON file in the app's
   config directory and **loaded automatically every time the app starts**
   — a save takes effect immediately, live, with no restart required, and
   is also broadcast to the main window so its own "Add target" form and
   theme update without a restart either.

Implementation: `core/include/netpulse/session.hpp` +
`core/src/session.cpp` (`set_default_recheck_tuning`/
`default_recheck_window_secs`/`default_recheck_threshold`, backed by two
atomics, replacing the old bare `kHopRecheckSecs`/`kLegacyMissThreshold`
constants at their three real call sites) → `netpulse_ffi.hpp`/`.cpp`
(`set_recheck_tuning`/`get_recheck_tuning_json`) → `ffi.rs`/`commands.rs`
(`AppSettings`, `load_app_settings`/`save_app_settings` reading/writing
the settings file and re-applying the tuning, `open_settings_window`
spawning the new window, `get_recheck_tuning` for a post-save readback) →
`tauri-bridge.js`/`SettingsWindow.jsx`/`App.jsx`. New
`tests/test_core.cpp` coverage: the default is 30s/2, `set_
default_recheck_tuning`'s clamping at both ends, and its "≤0 leaves that
field unchanged" convention.

**Also fixed while touching this area** (found only because a real
Rust/Tauri toolchain — `cargo check`/`cargo build`, previously
unavailable in every prior round's sandbox — was available this time):
`list_interfaces_detailed` (the Interfaces diagnostics page's data
source, added in an earlier round) was registered as a Tauri command and
granted in `capabilities/default.json`, but never added to `build.rs`'s
own `COMMANDS` list — the one place that actually makes tauri-build
generate the permission the capabilities file grants. Without this fix,
the Interfaces page's API call would have failed with a permission-denied
error in any real install; it was never caught before because no prior
round could actually build the Tauri app to notice.

Verified: full `netpulse_tests` suite (including three new
`set_default_recheck_tuning` cases) passes clean under both a normal
Debug build and a fresh ASan/UBSan build. `cargo check`/`cargo build`
(Rust host + cxx bridge to the C++ engine) succeed cleanly with zero
warnings — the first round in this whole engagement where that toolchain
was actually available, so the Rust/FFI side of this and prior rounds'
changes has now been compiled for real, not just reviewed. The exact
frontend (`.jsx`) changes were syntax/parse-verified with `esbuild`
(the sandbox's package registry allowlist blocks `xlsx`'s CDN
dependency, so a full `npm run build` isn't possible here) rather than
a real Vite build — disclosed plainly, not glossed over.

### A "stale, live via another target" hop was showing even with only one target running

Reported live: a hop marked stale (its address wiped after a route change)
displayed "live via another target" in its tooltip/badge, despite only one
target being active at the time — which should have been impossible if the
global shared-hop cache really means "another target". It was a real bug,
not a misreading: the cache's secondary "is this IP alive right now" index
(`SharedHopTable::ip_last_seen`) stored only a timestamp, with no record of
*who* published it. A hop's own last real reply — recorded right before it
goes stale, as `stale_since` — always falls inside the 30s freshness window
this check uses, so the very act of a hop going stale would routinely make
it look like it was "still being heard from elsewhere", even with a single
target and no other session ever having touched that IP.

Fixed by having `ip_last_seen` also record the publishing session's id
(`IpLastSeen{ts, owner_session_id}`), and having `shared_last_seen_from()`
(and its process-global wrapper `shared_last_seen()`) take the querying
session's own id and exclude entries whose owner is the caller itself —
mirroring the same owner-exclusion `shared_adopt_from()` has always done for
RTT adoption. The legitimate case (a genuinely different target's edge to
the same public IP still getting real replies via asymmetric/ECMP routing)
is unaffected and still surfaces correctly.

Verified with new `tests/test_core.cpp` cases: a single session publishing
and then querying its own last publish as "self" now correctly sees nothing
("no evidence of elsewhere"); a second, different session's publish to the
same IP is still correctly surfaced; `max_age` expiry still applies
regardless of owner. Full `netpulse_tests` suite re-run and passing.

### Force Recheck resumed a stale target quickly; the passive background self-heal ("auto refresh") also does it, but much more slowly — clarified, not changed

Reported live: after a route change left a hop stale, the manual Force
Recheck button resumed/re-verified it, but the passive background mechanism
that is supposed to do the same thing on its own ("auto refresh") appeared
not to. Traced through the current code (`core/src/session.cpp`): both
paths share the exact same underlying "guarded wipe" logic and the exact
same safety gate (`kGuardedWipeMaxRecentLossPct`/`kMaxGuardedWipesPerHop`) —
there is no separate, missing "auto refresh" feature. The difference is
purely timing, and it is a large one:
- **Force Recheck** arms a short burst of `kForceVerifyProbes` (3) probes
  sent at the session's **normal** probe cadence (1s by default) — a
  verdict lands within a few seconds of pressing the button.
- **The passive path** only sends one legacy TTL-elicited probe per
  `kHopRecheckSecs` (45s) on an already-confirmed hop, and requires
  `kLegacyMissThreshold` (2) of those misses spanning at least
  `kLegacyMissWindowSecs` (also 45s) before it will act — a minimum of
  roughly **90 seconds**, and often longer depending on when in that cycle
  the route actually changed, before the passive path even reaches a
  verdict, let alone wipes and rediscovers the hop.
That gap is a deliberate trade-off (a single probe's timing alone is not
reliable route-change evidence — see the code's own `kLegacyMissWindowSecs`
rationale), not an oversight, but it fully explains "force refresh did it,
auto refresh did not [yet]": whoever checked did so well inside that ~90s
window. No code change was needed here — this is a documentation/behavior
clarification only, captured in `ARCHITECTURE.md` §6 and `cli/CLI.md`/the
GUI help text so this asymmetry is no longer surprising.

### `tracert-mtr`/`ifconfig -w` could corrupt the parent shell's prompt (PowerShell specifically), plus a visible "you're in npulse" indicator

Reported live: running either directly from an already-open PowerShell
session (not through the `npulse` console, which hosts `cmd.exe` and was
unaffected) could leave the prompt corrupted afterward — text fragments
appearing to type themselves, spurious continuation prompts, arrow keys
inserting garbage. Root cause: a live-redraw view repositioning the
cursor inside the same screen buffer a line editor (PSReadLine) is also
tracking desyncs that editor's bookkeeping from the real cursor position.
Fixed with the standard technique: `tracert-mtr`'s live view and
`ifconfig -w` now switch to the alternate screen buffer
(`\033[?1049h`/`\033[?1049l`) once for the whole session instead of
repositioning the cursor in the shell's own buffer — the same guarantee
`vim`/`less`/`htop` give on quit, on every exit path including Ctrl-C.
Two further defensive layers: the Windows console mode this tool enables
is now saved and restored via `atexit()` rather than left changed after
exit, and any input the terminal sent but this process never read is
explicitly discarded on exit (`FlushConsoleInputBuffer`/`tcflush`).

Also added the visible indicator asked for: a window/tab title
(`npulse console`, or `npulse tracert-mtr HOST` for a directly-run live
view) via `SetConsoleTitleA`/the OSC 0 escape sequence, and — inside the
`npulse` console specifically — the spawned shell's own prompt prefixed
with `[npulse] ` (a `PROMPT` env var for `cmd.exe`; bash/zsh needed
`--rcfile`/`ZDOTDIR` specifically, since a plain inherited `PS1` gets
unconditionally overwritten by the shell's own startup file before the
first prompt ever shows — confirmed by testing a genuinely interactive
shell, not assumed).

Verified with a real pseudo-terminal on Linux: captured the actual escape
bytes (not just visual inspection) confirming the alt-screen sequence and
title are correct, confirmed the `[npulse]`-prefixed bash prompt renders
live, confirmed a real SIGTERM mid-session completes promptly (~3s, not a
hang). Re-verified on the cross-compiled Windows `.exe` under Wine across
multiple runs. Two testing-tool artifacts surfaced and were resolved by
reproducing in isolation rather than assumed to be real bugs: a hang
traced to the `script` pty-recording utility's own quirks (not this code
— confirmed by removing it and re-running), and one non-reproducible Wine
flake (three immediate reruns clean).

### `npulse` console: `ping`/`tracert`/`mtr`/etc. weren't actually runnable inside it — now they are

Reported live: `tracert-mtr host` typed inside the console failed with
"not recognized as an internal or external command." Root cause: the
console's own banner promised "use `ping`/`tracert`/`mtr`/`ifconfig`
directly," but nothing ever created a file by any of those names — the
`argv[0]`-dispatch logic only activates for a name that exists somewhere
on PATH, and the console only ever prepended `npulse`'s own directory
(containing just `npulse` itself). Fixed: entering the console now also
creates a session-scoped temporary directory of hard links to the
`npulse` executable under each standard-command name (`ping`, `tracert`,
`traceroute`, `mtr`, `tracert-mtr`, `ifconfig`, `ipconfig`, `nslookup`),
prepended to PATH, and removed automatically when the session ends. Also
added `tracert-mtr` itself as a recognized `argv[0]` alias (previously
only `mtr` was). This only shadows the system's own tools for the
lifetime of the one console explicitly opened for this purpose — the
same reasoning `conda activate`/a virtualenv already rely on — not a
permanent PATH change.

Verified precisely against the report: on Linux, confirmed every alias
name resolves and runs correctly from inside a spawned session, confirmed
the alias directory is fully cleaned up afterward, clean under ASan/UBSan.
On the cross-compiled Windows `.exe` under Wine: confirmed hard-link
creation and a directly-invoked alias binary both work correctly in
isolation; the full nested chain (console spawns a shell, which spawns an
alias binary) hung reliably under Wine specifically despite every
individual piece checking out — most consistent with a Wine limitation,
but not confirmed against real Windows, and documented as an open
question in `CLI.md` rather than assumed away.

### `tracert-mtr`: a long hostname could hide its own `[DEST]` marker

Reported live, alongside the above: the destination marker was appended
to a hop's display text before truncating it to the Host column's width,
so a long (routinely long, for real IPv6 reverse-DNS names) hostname
could cut the marker off partially or entirely — hiding exactly the
information that matters most, on exactly the row most likely to trigger
it. Fixed by truncating the hostname/address text first, reserving room
for the marker, then appending it after — it can no longer be truncated
away. Also widened the Host column (38 → 46 characters) since the same
report showed truncation is the common case for real hostnames, not a
rare edge case. Also fixed two remaining em-dash characters (in the
Auto-family retry message and the `ifconfig -w` refresh message) that a
final sweep had missed in an earlier mojibake fix — same class of bug,
caught proactively this time rather than waiting for another report.

### `npulse` console: clarified to trigger uniformly on every OS and invocation style, not just a fresh Windows console

Second-round clarification: the console-hosting feature (VS Developer
Command Prompt style — spawn the user's real shell with `npulse`'s own
directory on PATH) was meant to trigger whether `npulse`/`netpulse` is
typed bare into an already-open terminal OR the file is run directly, on
every OS — not gated on Windows' "was this console freshly allocated"
distinction the way the first version was (which left an already-open
shell's bare `npulse` still just showing help). Removed that gate;
`launch_shell_console()` now runs on any zero-argument invocation of the
canonical `npulse`/`netpulse` name, on Windows (`%COMSPEC%` via
`CreateProcessA`) and POSIX (`$SHELL`, falling back to `/bin/sh`, via
`fork()`/`execl()`) alike. An alias invocation (`ping`, `tracert`, `mtr`,
`ifconfig`) with no arguments is unaffected — that alias's own usage
applies, unchanged.

Verified completely on Linux this time, not just cross-compiled: a bare
invocation spawns the real `$SHELL` (confirmed explicitly with `/bin/sh`
and `/bin/bash`), the spawned shell's `PATH` genuinely has `npulse`'s
directory prepended (`which npulse` resolves it), `npulse help` runs
correctly from inside that session, the spawned shell's exit code
propagates back correctly, and every alias with no arguments correctly
keeps its own behavior. Also re-verified on the cross-compiled `.exe`
under Wine, repeated multiple times for stability after one transient
false alarm during testing (traced to Wine flakiness, not a real bug, by
first reproducing the exact `CreateProcessA` pattern in isolation and
confirming it worked correctly on its own).

### New: `npulse.exe` opened directly (Windows) hosts a real shell, VS Dev Command Prompt style

Clarified request: double-clicking `npulse.exe` (or a shortcut to it) —
i.e. no arguments, no existing console — should open a console that stays
open, running the user's actual shell (`cmd.exe`) with `npulse`'s folder
on `PATH` for that session, exactly like "VS Developer Command Prompt" is
genuinely just `cmd.exe` with an environment script run first, not a
custom shell of its own. Implemented via `GetConsoleProcessList()`
reporting exactly 1 attached process — the standard, Microsoft-documented
way to tell "this console was freshly allocated for me" apart from "I was
run from an already-open shell" (confirmed against Microsoft's own
official guidance, not assumed) — so `npulse` typed with no arguments into
an existing cmd/PowerShell/Windows Terminal session is completely
unaffected and still just prints help as before.

Also installed a MinGW-w64 cross-compiler and Wine in this environment
specifically to verify this and the rest of the CLI's Windows-specific
code properly: cross-compiled the entire engine + CLI for a real Windows
target and ran the resulting `.exe` under Wine, confirming real `ping`,
`ifconfig` (via actual `GetAdaptersAddresses`), and `dns` lookups all work
correctly against real network traffic — substantially stronger
verification than the compile-only/manual-review approach used for
Windows-specific code earlier in this project's history. One caveat:
Wine's console emulation doesn't faithfully reproduce the exact
`GetConsoleProcessList()` count a genuine Windows double-click produces,
so that one specific boundary condition still needs a real Windows machine
to fully confirm — noted plainly in `CLI.md` rather than claimed as fully
verified.

### `tracert-mtr` garbled ("Γÿà"), and hops showed `*` next to real, good numbers

Reported live: the destination row's marker rendered as literal garbage
("Γÿà" instead of "★"), and a route-change wipe briefly left a hop showing
`*` (unresolved) alongside a full set of healthy loss/RTT stats, which
reads as a broken/inconsistent row.

Both fixed at the root, not patched around:

- **Mojibake**: "★"/"…"/"⚠" were UTF-8 multi-byte sequences. A Windows
  console not explicitly in UTF-8 output mode reads each byte of a
  multi-byte character as a SEPARATE legacy-codepage character — exactly
  what "Γÿà" is. Replaced all three with plain ASCII (`[DEST]`, `...`,
  `!`) — the correct, guaranteed-portable fix, not a font/codepage
  workaround — plus added `SetConsoleOutputCP(CP_UTF8)` defensively for
  anything else (a reverse-DNS hostname, say) that could still legitimately
  contain non-ASCII bytes. Also caught and fixed the same class of issue in
  `print_help()`'s em-dashes, which hadn't been reported but would have hit
  the identical bug.
- **`*` next to good stats**: a hop the guarded-wipe/Frankenstein-route
  guard just cleared (ARCHITECTURE.md §6) can briefly show real, recent,
  healthy numbers even though its address field is now empty — the wipe
  only ever fires on a hop that WAS looking healthy, so the stats window
  still has genuinely good recent samples in it right after. Previously
  rendered as bare `*`, indistinguishable from a hop that's never resolved
  at all. Now shows `(re-resolving)` specifically when there's an address
  gap WITH real recent data, vs. plain `*` for a hop with no data at all —
  the same underlying (correct, intentional) engine behavior, made
  self-explanatory instead of looking like a rendering bug.

Also matured `ping` and `tracert` to match `tracert-mtr`'s presentation
level rather than staying plain: color-coded replies/RTT-severity/loss
percentage (green/yellow/red, the same thresholds used everywhere else),
bold banners and summary lines, protocol/payload/probe-count shown in
`ping`'s banner, a `[DEST]`-tagged, colored final row in `tracert`. All
fully suppressed automatically the instant output isn't a real terminal
(piped/redirected), exactly like the rest of this tool's color handling.

Verified: full rebuild, `netpulse_tests` clean (plain + ASan/UBSan);
grepped the actual binary's output for any remaining non-ASCII byte across
every command (zero found); re-ran `ping`/`tracert`/`tracert-mtr` under a
real pseudo-terminal (Linux `script`) to confirm color renders correctly
and no control-code or encoding artifacts leak into either the live or
piped paths.

### `tracert-mtr` printed a new table every refresh instead of updating in place (real Windows report)

Reported live against Windows PowerShell (`powershell.exe`, not `pwsh.exe`):
every redraw scrolled a fresh copy of the table below the last one. Root
cause: a Windows console only interprets ANSI/VT escape codes once a
process explicitly opts in via `SetConsoleMode(...,
ENABLE_VIRTUAL_TERMINAL_PROCESSING)` — classic `powershell.exe`, unlike
Windows Terminal/PowerShell 7, doesn't do this automatically, so the escape
codes being printed were just inert bytes and every redraw's output simply
appended below the last. Separately, even once VT mode is enabled, a full
`\033[2J\033[H` screen clear every single frame (what this used) is its own
real UX problem — visible flicker on every refresh — independent of the
VT-mode bug.

Fixed properly, not worked around: `term_init()` explicitly enables VT
processing on Windows and checks for a real terminal (not piped/redirected)
before enabling any ANSI feature; the live views (`tracert-mtr`,
`ifconfig -w`) no longer clear the whole screen every frame at all — they
move the cursor home and overwrite in place (clearing only a leftover
trailing line or shorter final frame), the same flicker-free technique
`mtr`/`htop`/SolarWinds Traceroute NG use. `-r`/`--report`/`--json` output
now never includes any redraw control codes, even when run attached to a
real terminal rather than piped (color is separate and still applies
there, exactly like `git status`/`ls --color=auto`, fully suppressed the
instant output isn't a real terminal).

Also, per the same report: the table's visual presentation was made more
professional — bold headers, a clean separator rule, green/yellow/red
loss-percentage coloring matching the GUI's own severity bands, a ★
destination marker matching the GUI's, and long hostnames truncated with
`…` instead of breaking column alignment. Applied consistently to both
`tracert-mtr` and `ifconfig -w` (the latter previously used a different,
older, uncolored rendering path entirely).

Verified: captured the raw bytes emitted under a real pseudo-terminal
(Linux `script`, since this environment has no real Windows/interactive
session to test against directly) and confirmed the exact escape sequence
is correct (`\033[H` once per frame, `\033[K` per line, `\033[J` once at
frame end) — not just visually, but at the byte level; confirmed report
mode emits zero redraw codes under the same real-terminal test while still
showing color; confirmed color is fully suppressed the instant output is
piped instead. Full rebuild and `netpulse_tests` clean (plain + ASan/UBSan)
throughout.

### CLI matured toward GUI parity: split `tracert`/`tracert-mtr`, added iface/timer options, auto-refresh

`trace` split into two commands matching two genuinely different real-world
tools rather than one command with a mode flag: `tracert` (also
`traceroute`) is now a classic one-shot, progressive route trace — prints
each hop once, in order, as it settles, stopping at the destination or
`--max-hops`, matching Windows `tracert`/Linux `traceroute`'s output shape;
`tracert-mtr` (also `mtr`) is the live, continuously-refreshing table the
old `trace` command was.

Both gained the rest of the GUI's Add Target option set that was missing
before: `-T`/`--trace-interval` (`Settings::trace_interval`, the GUI's
"Trace" field — route re-discovery interval, distinct from `-i`'s per-hop
probe interval), `-W`/`--timeout`, `-s`/`--payload`, `-I`/`--interface`
(`Settings::source_addr`, the GUI's "iface" dropdown), and
`--unprivileged` (the inverse of the GUI's "Raw" checkbox). `ping` gained
`-I`/`--interface` too.

**Auto-refresh, as the Force-Recheck alternative asked for**: `Family:
Auto` in `tracert-mtr` now self-heals a stuck "no local egress" state
automatically — `Session::resolve()` only ever runs once, at session
start, so a trace begun before a real route came up used to just sit there
showing a stale error forever. Now, under `Auto` specifically, a
persistent error auto-restarts the session (re-running family detection)
every few seconds until it resolves or Ctrl-C. A pinned `-4`/`-6` never
does this. `ifconfig` separately gained `-w`/`--watch` for continuously
monitoring the adapter list itself (a cable unplugged/replugged, a VPN
connecting) — the interfaces-specific counterpart to the same idea.

Verified: full rebuild and unit-test suite clean (plain and ASan+UBSan);
every new/changed command run against real traffic, including the
auto-refresh restart loop specifically (confirmed it retries under `Auto`
family, confirmed a pinned family does NOT retry, confirmed Ctrl-C still
stops the retry loop immediately with no thread-safety issues under ASan
from the internal poller thread this needed to combine "real interrupt"
and "internal restart decision" into the one stop signal `Session::run()`
accepts).

### `tauri dev`/`cargo build` failed outright: "resource path binaries\npulse-... doesn't exist"

Reported against a real Windows dev run — `bundle.externalBin` makes
`tauri_build::try_build()` (`build.rs`) hard-fail immediately, before
compiling anything, if the per-target sidecar file it names isn't already
on disk. The CI workflows each have their own step that builds it before
`tauri build` runs, so releases were never affected — but a plain local
checkout has no such step, so this broke `tauri dev` for literally anyone
building the project for the first time, which is about as bad as a
regression gets. Fixed by having `build.rs` build the CLI sidecar itself,
automatically, via the same top-level `CMakeLists.txt`/`cmake` invocation
the rest of this file already uses for the C++ engine — before calling
`tauri_build::try_build()`, and skipped entirely on every build after the
first (checks whether the target file already exists). If `cmake`/a C++
compiler genuinely isn't available, this now fails with a clear, actionable
message (exact commands to run manually) instead of the cryptic Tauri
internal error above. Verified as thoroughly as this environment allows: no
Tauri/cargo toolchain here to compile the real `build.rs` end-to-end, but
the exact new function was extracted into a standalone Rust program,
compiled and run for real against the actual repository (fresh build, then
a second skip-if-exists run, then a simulated `cmake`-missing failure to
confirm the panic message), and the complete `build.rs` was syntax/type-
checked with `rustc` directly — the only errors are the two external crates
(`tauri_build`, `cxx_build`) this sandbox genuinely can't provide, nothing
in the new or reordered code.

### CLI: standalone-runnable confirmed, published as a separate download too, dev commands documented

Confirmed the CLI has zero dependency on the GUI/Tauri stack — `ldd` on the
built binary shows only `libc`/`libstdc++`/`libm`/`libgcc_s`, nothing
GTK/WebKit/X11/Wayland-related — so it runs as a fully standalone process on
a machine that will never install the desktop app: a headless server, a
minimal/no-desktop Linux install (Arch and similar), Windows without the
GUI app installed at all. This project's whole CLI development and testing
this round was already done in exactly such an environment (a headless
Linux container), which is itself a real demonstration of this working, not
just a claim.

Added: the CLI sidecar is now ALSO published as its own standalone download
on the GitHub Release (`npulse-<os>[.exe]`), separate from every installer —
this project has no native Arch package (no PKGBUILD/AUR) and no way to put
the CLI on PATH from an AppImage (no install step exists for that format at
all), so a plain per-OS binary download is the practical way to get just the
CLI there today. `README.md` now documents this plus explicit dev-mode
commands for running the GUI and the CLI independently (`npm run dev` for
frontend-only, `npx tauri dev` for the full GUI, `cmake --build build
--target npulse` for the CLI alone — no Node/Rust/Tauri needed for that
last one).

### New: netpulse-cli (`npulse`) — a real, tested CLI, plus standard-command aliases

Added `cli/main.cpp` and `cli/CMakeLists.txt`: a genuine, built, and
tested command-line tool reusing the exact same engine (`Session`,
`PingRun`, `list_interfaces`) the desktop app uses — no new probing logic.
`argv[0]`-based dispatch means a copy/symlink of the binary named `ping`,
`tracert`, `traceroute`, `mtr`, `ifconfig`, `ipconfig`, or `nslookup`
behaves like that command directly; the canonical form is `npulse
ping|trace|ifconfig|dns|portscan|completion HOST [options]`. Live-tested
against real traffic on the machine this was built on (ping/trace against
a real gateway, dns against real records, portscan against real ports,
argv[0] aliasing confirmed by literally copying the binary under each
alias name and running it) and clean under ASan+UBSan across every
subcommand. Full reference: `cli/CLI.md`; architecture rationale:
`ARCHITECTURE.md` §11.

Also added: `npulse completion bash|zsh|fish|powershell` (shell
auto-completion scripts, the same pattern `kubectl`/`docker` use).

### New: CLI bundled into every OS installer via a Tauri `externalBin` sidecar

`tauri.conf.json` now declares `bundle.externalBin`, and each of
`tauri-ci.yml`/`tauri-release.yml`/`tauri-canary-build.yml` builds the CLI
for that job's exact OS/architecture and places it where Tauri's sidecar
convention expects before packaging — see `tauri-app/src-tauri/binaries/
README.md`. Windows gets the CLI added to the current user's PATH via an
extended `windows/hooks.nsh` (NSIS `EnVar` plugin, plus a pre-install
`taskkill` working around a real, documented Tauri/NSIS externalBin
reinstall gotcha); Linux `.deb` gets it placed at `/usr/bin/npulse`
directly via `bundle.linux.deb.files` (dpkg handles PATH automatically, no
script needed). **Honest scope note**: the CLI binary itself is
build-verified (compiles, runs, passes ASan) on the machine this was
built on; the actual Tauri/cargo packaging pipeline and the Windows/Linux
installer behavior described above could not be run end-to-end in that
same environment (no Tauri toolchain, no real Windows/macOS runner) —
written to match Tauri's and NSIS/EnVar's documented conventions as
closely as possible, and cross-checked against Tauri's own current
documentation and a real reported issue for the exact sidecar-reinstall
gotcha addressed, but flagged as unverified rather than presented as
tested. macOS `.dmg`/`.app` bundling works the same way but has no
PATH-registration step at all yet (a `.dmg` has no install-time script
execution point) — noted as a known gap with a concrete proposed fix (an
in-app "install to PATH" action) in `CLI.md`'s Packaging section.

### `Family: IPv6` treated link-local-only machines as having real IPv6 egress

Found while specifically re-checking IPv6 correctness. `Session::run()`'s
"does this machine have a usable local IPv6 egress" check
(`has_local_v6`) counted **any** IPv6 address on **any** active interface
as proof of usable egress — including link-local (`fe80::/10`). Link-local
addresses are auto-assigned to every active interface by SLAAC on every
major OS regardless of whether the machine has any real route to the
internet at all, so this check passed on virtually every machine, IPv6
connectivity or not. With `Family: IPv6` explicitly selected on a
link-local-only (i.e. actually IPv6-less in practice) machine, this meant
the engine skipped straight past the "No local IPv6 egress available —
waiting for IPv6 or change family" message — the exact case that message
exists to catch — and instead proceeded to open a real IPv6 socket and
attempt real probing to a destination it could never actually reach,
producing confusing 100%-loss/no-discovery behavior instead of the clear
wait-and-explain message. The IPv4 side of the same check had the
equivalent gap for `169.254.0.0/16` (APIPA) — much rarer in practice
(IPv4 only self-assigns link-local when DHCP fails) but fixed identically
for consistency. Fixed by filtering interface addresses through
`is_cacheable_ip()` (already excludes link-local, loopback, and
unspecified addresses while still correctly accepting private/CGNAT ones —
a home LAN behind NAT is a perfectly real egress) before counting them
toward `has_local_v4`/`has_local_v6`. Added direct unit-test coverage for
`is_cacheable_ip()` itself (`tests/test_core.cpp`) to lock this in.

### IPv6 audit: checksum/type-code/packet-parsing confirmed correct, one gap found and fixed above

Went through every IPv6-specific code path in `core/` line by line, since
IPv6 issues can't be live-tested from this environment (see the
Verification section, `CHANGES.md`) and are easy to miss otherwise:
ICMPv6 type codes (128/129 Echo Request/Reply, 3 Time Exceeded, 1
Destination Unreachable — all correct per RFC 4443, and distinct from
ICMPv4's 8/0/11/3), the deliberate omission of ICMPv6 checksum computation
in `build_echo()` (correct — RFC 3542-compliant raw ICMPv6 sockets have the
kernel compute and overwrite it regardless of what userspace writes; already
documented in `ARCHITECTURE.md` §5 as the reason Paris-traceroute-style
checksum pinning is IPv4-only), the no-IP-header-prepended raw-socket
convention `parse_v6()` correctly assumes (unlike `parse_v4()`, which does
expect one), and `IP_TTL`/`IPV6_UNICAST_HOPS` branching in all three of
`transport.cpp`/`probe_tcp.cpp`/`probe_udp.cpp` (HTTP mode reuses
`ProbeTcp` internally, so it inherits this for free). All confirmed
correct — the link-local-egress-detection bug above was the one real gap
found.

### Adding a hostname under a different IP family was rejected as a duplicate

`addTarget()`'s own duplicate-check comment said trace identity is `(host,
protocol, port)` and explicitly called "IPv4 vs IPv6" a legitimately
different measurement — but the actual comparison had dropped `family` from
the tuple, so adding the same host as v4 and then v6 got rejected as
"already in the list." Separately, `addTargetHost()` (the quick-trace menu
shortcuts) hardcoded `family: 'auto'` on every call and deduped on hostname
alone, so it could never add a second family for a host at all. Fixed: family
is back in the identity tuple, compared via each target's *resolved* family
with a fallback to the configured label while unresolved; `addTargetHost`
takes a real `family` parameter. Also added: adding on Family=Auto when the
host already has exactly one family being traced now steers the new request
at the *missing* family instead of silently duplicating the one already
there.

### Force Recheck could only nudge the Frankenstein-route guard, never resolve it

`force_recheck()` used to zero `hop_recheck_at`, firing exactly one legacy
probe — it's reset to `now` the instant it's sent — so a single click could
contribute at most one miss toward the passive guard's
`kLegacyMissThreshold`-misses-over-`kLegacyMissWindowSecs` (≈45s) gate,
never enough on its own to decisively confirm or clear a stuck hop. Fixed:
Force Recheck now seeds a burst of `kForceVerifyProbes` (3) back-to-back
legacy probes at normal probe cadence (completes in a couple of seconds).
If every probe in the burst misses, that's treated as sufficient real-time
evidence (several clustered misses rule out ordinary jitter as well as the
wall-clock-separated pair does) and the same guarded wipe fires immediately,
through the same safety gates. See `ARCHITECTURE.md` §6 for the full
mechanism.

### Private/CGNAT hops were never cross-target-cached, even when safe to be

The shared-hop cache (`SharedHopTable`) excluded every private/CGNAT
responder IP outright, because the cache key's `source` component
(`source_addr`) is usually an empty string — most targets never explicitly
bind an egress interface — which can't disambiguate two sessions actually
routed through different physical interfaces. Fixed: added
`local_egress_ip()` (`transport.cpp`), the standard "UDP `connect()` trick,"
to determine the real local egress address for a destination with zero
configuration required; private IPs are now cacheable whenever that (or an
explicitly configured `source_addr`) is available, and fall back to the
original public-only behavior only when it can't be determined. See
`ARCHITECTURE.md` §4.

### Protocol-parity bug found while verifying the above: UDP/TCP/HTTP used an unsafe predecessor lookup ICMP had already been fixed away from

While confirming the shared-hop cache fixes apply identically across all
four probe protocols, found that `predecessor_of()` — which decides the
cache key — was implemented two different ways in the same codebase: the
ICMP loop used a safe, deliberately-argued-for version (hop-1 only, a
per-depth `"UNKN-<h>"` sentinel when unresolved), while UDP, TCP, and HTTP
all still used an older "walk back to the nearest resolved hop" version
that the ICMP code's own comment explicitly warns against — it can make two
targets that have genuinely diverged share one cache entry and stomp each
other's real measurements. Not introduced by the fixes above, but exposed
by verifying them; all four protocols now use the identical, safe logic.

## 1.1.2

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

### Release pipeline: `latest.json` never existed at all, regardless of signing

With the macOS build fixes above landing, the first real release run (all
three OS installers built successfully) still failed in the publish job, at
"Merge per-OS updater manifests into one latest.json": zero manifests found.

The first diagnosis here was wrong, and worth recording honestly rather
than quietly editing out: `CODE_SIGNING.md`'s own heading ("future
implementation") made it look like a missing signing key was the cause, and
a first fix made the merge step tell apart "no key configured" (expected)
from "key configured but still failed" (a real problem) — reasonable
handling for a genuinely different bug, but not this one. A real build log,
requested specifically to settle it, proved that theory wrong directly:
signing had genuinely succeeded (`Finished 1 updater signature at
...exe.sig`), yet the very next line from `tauri-action` itself was `No
releaseId or tagName provided, skipping all uploads...`.

Tracing that message into `tauri-action`'s own source confirmed the actual
root cause: `latest.json` is never a file `tauri build` writes to disk at
all — `createUpdaterArtifacts: true` only makes it emit the `.sig`
signature files. `latest.json` is constructed by `tauri-action` itself,
entirely in memory, exclusively as part of its own upload-to-release logic,
which requires `tagName`/`releaseId`. This project's build job never
passed either (deliberately, on the theory that `latest.json` was some
other local build output the "Collect installer files" step further down
could just find and copy — it never could, since it never existed on disk
to begin with). No version of the signing key, its password, or the merge
script downstream could ever have produced a `latest.json`, regardless of
how correct any of their own logic was, because `tauri-action`'s manifest-
generation code path was never being reached at all — with or without a
signing key.

Fixed by switching to `tauri-action`'s own documented pattern for a multi-
OS matrix release: `GITHUB_TOKEN` + `tagName` + `releaseDraft: true`,
letting each of the three concurrent OS jobs safely share one draft release
that `tauri-action` finds-or-creates and merges every platform's entry into
one real `latest.json` natively — the exact job the project's own custom
`merge-latest-json.mjs` script was trying to reimplement, and could never
succeed at no matter how correct its own logic was, since it depended on a
local file that was never going to exist. That script is now genuinely
unused and was removed rather than left as dead code.

This changed the shape of the whole release pipeline, not just one step —
`softprops/action-gh-release` (the tool that used to create the release)
was removed too: with the build job now creating the release itself via
`tauri-action`, asking a second, different tool to "create or update" the
same tag has real, documented failure modes for exactly this scenario
(`softprops/action-gh-release` issues #445 and #403, both `already_exists`
against a release created by something else — and the fix for reusing an
existing draft correctly only landed in v3.0.2, while this file was pinned
to v2). Replaced with the official `gh` CLI directly for the publish job's
final step: `gh release upload` unambiguously requires an already-existing
release (no create-vs-update ambiguity at all), and `gh release edit
--draft=false` reliably publishes it.

Two real mistakes were caught and fixed while making this change, both
before it shipped: a `str_replace` edit that inserted the new, correctly-
configured `tauri-action build` step without removing the old one, briefly
leaving two copies of the same step in the file (caught by re-grepping for
step names immediately after the edit, not assumed correct); and a job-
level `permissions: contents: write` block that would have silently
*removed* the `id-token`/`attestations` permissions the same job's build-
provenance-attestation step depends on, since GitHub Actions job-level
permissions override the workflow-level ones rather than adding to them,
not something either would have been flagged by YAML validation alone.


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

The work below shipped as part of the 1.1.2 release above rather than
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
# netpulse-cli (npulse) — command reference

> **Status: built and tested locally, not yet verified through the full
> Tauri packaging/release pipeline.** `cli/main.cpp` exists, builds cleanly,
> and every command below has been run against real traffic and passes
> clean under AddressSanitizer + UndefinedBehaviorSanitizer — see
> `CHANGES.md`'s verification sections for the exact commands run. The
> console-hosting feature (see below) was fully tested end-to-end on
> Linux directly, including the exact "tracert-mtr not recognized" failure
> a real Windows run reported and its fix. Windows-specific code has been
> cross-compiled with `x86_64-w64-mingw32-g++` for a real Windows target
> and actually run under Wine (not just compiled) — confirmed real `ping`,
> `ifconfig` via `GetAdaptersAddresses`, `dns` lookups, hard-link creation,
> and a hardlinked alias binary invoked directly all work correctly. One
> specific scenario — the full nested chain of `npulse.exe` spawning a
> shell that itself spawns an alias binary — hung reliably under Wine
> despite every individual piece checking out in isolation; this could not
> be confirmed on a real Windows machine and is flagged plainly in "The
> npulse console" section below rather than assumed away. Still not a
> substitute for testing on a real Windows machine. What's **not** verified
> at all: the actual `tauri
> build`/GitHub Actions sidecar-bundling pipeline that ships this inside
> each OS's installer (no Tauri/cargo toolchain or real Windows/macOS
> runner in the environment this was built in). Separately, the terminal-
> corruption fix and visible "you're in npulse" indicator (see "Terminal
> corruption" below) were verified with a real pseudo-terminal on Linux
> (captured escape bytes directly, not just visual inspection) and on the
> cross-compiled Windows `.exe` under Wine across multiple repeated runs,
> including graceful `SIGTERM` handling mid-session.

`npulse` is a terminal front-end over NetPulse's own probe engine
(`core/`) — the same `Session`/`PingRun`/`list_interfaces` calls the Tauri
desktop app uses, exposing close to the GUI's full option set (family,
protocol, port, max hops, probe interval, route re-discovery interval,
timeout, payload size, source interface, raw/unprivileged) rather than a
stripped-down subset. No probing logic lives in `cli/` — only argument
parsing and terminal rendering.

## The `npulse` console — a real shell, on every OS, "VS Developer Command Prompt" style

Running `npulse`/`netpulse` with **zero arguments** — typed bare into an
already-open terminal, OR the file run directly (double-clicked from
Explorer/Finder, a Start Menu/desktop/file-manager shortcut, `./npulse`) —
opens (or takes over) a console and spawns your own real shell: `cmd.exe`
on Windows (via `%COMSPEC%`), or `$SHELL` on Linux/macOS (falling back to
`/bin/sh`). Type `npulse help`, or `ping`/`tracert`/`traceroute`/`mtr`/
`tracert-mtr`/`ifconfig`/`ipconfig`/`nslookup` directly, alongside any
normal command for your OS — it's your real shell the whole time, not a
custom REPL of its own. This mirrors "VS Developer Command Prompt" exactly:
that shortcut is genuinely just `cmd.exe` with an environment script
(`vsdevcmd.bat`) run first — same idea here, just a different environment
tweak. Type `exit` (or Ctrl-D) to end the session, same as any shell.

**BUG FIX, reported live**: the first version only prepended `npulse`'s
own directory to `PATH` — which makes `npulse` itself runnable by name,
but does nothing for `ping`/`tracert`/`mtr`/etc., since those names only
dispatch correctly (via `argv[0]`) when a file that's actually *named*
that way exists somewhere on `PATH` — nothing ever created one. Typing
`tracert-mtr host` inside the console failed with "not recognized as an
internal or external command," exactly as reported. Fixed: entering the
console now also creates a session-scoped temporary directory containing
a hard link to the `npulse` executable under each standard-command name
(`ping`, `tracert`, `traceroute`, `mtr`, `tracert-mtr`, `ifconfig`,
`ipconfig`, `nslookup`) and prepends *that* to `PATH` too — a hard link
costs zero extra disk space (the same file, one more directory entry) and
needs no elevated privilege, unlike a symlink on Windows. The directory
and its contents are removed automatically the moment the session ends
(you type `exit`) — this only ever shadows the system's own `ping`/
`tracert`/etc. for the lifetime of the one console you explicitly opened
for exactly this purpose, the same reasoning `conda activate`/`nvm use`/a
Python virtualenv already rely on to temporarily shadow commands within
one activated session without touching the system permanently. This is
deliberately different from a *permanent* install — see "Packaging" below
for why those names are never installed to PATH outside this console.

**Only for the canonical `npulse`/`netpulse` invocation, and only with
zero arguments.** `npulse help`, `npulse <anything>` — any argument at all
— is unaffected and behaves exactly as documented below. Running under an
**alias** name (inside the console, or a copy/symlink made outside it)
with no further arguments is a *different* case: that alias's own usage/
behavior applies (e.g. bare `ping` shows `ping`'s usage message) — you
asked for that specific tool, not npulse's general console.

**Verification, precisely scoped**: fully tested end-to-end on Linux in
the environment this was built in, including the exact reported failure
mode — confirmed `which tracert-mtr`/`ping`/`tracert`/`mtr`/`ifconfig`
inside a spawned session all resolve to the new alias directory, confirmed
running `tracert-mtr HOST` directly by name inside that session produces
correct, fully-formatted output, confirmed the alias directory is deleted
after the session exits (no leftovers), and confirmed the exit code and
existing per-alias usage-message behavior are all unchanged. All clean
under AddressSanitizer/UndefinedBehaviorSanitizer.

For Windows: a MinGW-w64 cross-compiler and Wine were used to build and
run the real `.exe`. Confirmed independently, in isolation, that
`CreateHardLinkA` itself works correctly under Wine, and that a renamed/
hardlinked copy of the executable invoked *directly* under an alias name
(bypassing the nested-shell setup) dispatches and runs correctly,
including a full `tracert-mtr` run with correct `[DEST]`-marked output.
**What did not work under Wine**: the full nested chain — `npulse.exe`
spawning `cmd.exe`, which then spawns the hardlinked `tracert-mtr.exe` —
hung reliably in this environment. Every individual piece of that chain
was verified working correctly in isolation (hard-link creation, `cmd.exe`
spawning and exiting cleanly on its own, the alias binary run directly),
which points at a Wine-specific limitation in its console/process
emulation for a process spawning a shell that itself spawns a further
process doing raw-socket networking, rather than a defect in this code —
but this could **not** be confirmed on a real Windows machine, and is
flagged plainly rather than assumed away. If `ping`/`tracert`/`mtr`/etc.
typed inside the console hang or behave oddly on an actual Windows
machine, this nested-invocation depth is the first place to look, and
running `npulse tracert-mtr HOST` (the subcommand form, no nesting) is a
safe fallback that reuses none of the code this specific concern is about.

## Terminal corruption after `tracert-mtr`/`ifconfig -w` (fixed), and a visible "you're in npulse" indicator

**BUG FIX, reported live against PowerShell specifically**: running
`tracert-mtr`/`ifconfig -w` directly from an already-open PowerShell
session (not through the `npulse` console above) could leave the PROMPT
ITSELF corrupted afterward — fragments of unrelated text appearing to
type themselves, spurious continuation prompts, arrow keys inserting
garbage instead of navigating. This is a well-known class of Windows-
console pitfall, not specific to this program: a live-redraw view that
repositions the cursor inside the SAME screen buffer a line editor
(PowerShell's PSReadLine, or any shell's own line editing) is also
tracking desynchronizes that editor's bookkeeping from where the cursor
actually is — and once desynced, ordinary typed input can render or get
interpreted incorrectly. `cmd.exe`, which is what the `npulse` console
above hosts, doesn't have this problem, which is exactly why it was
reported as unaffected while direct-from-PowerShell usage was.

Fixed with the standard, well-known technique for exactly this: `tracert-
mtr`'s live view and `ifconfig -w` now enter the **alternate screen
buffer** (`\033[?1049h`) once before the session starts, do all per-frame
redrawing entirely inside that separate buffer (which no line editor is
tracking, because it isn't the buffer the shell's prompt lives in), and
leave it (`\033[?1049l`) once the session ends — which atomically restores
the terminal to exactly the scrollback and cursor position it had before,
the same guarantee `vim`/`less`/`htop` already give when you quit them.
This happens on every exit path, Ctrl-C included, not just a clean one.

Two further defensive layers, since this class of bug is worth being
thorough about rather than fixing only the one symptom that got reported:
the console mode this tool enables on Windows (`ENABLE_VIRTUAL_TERMINAL_
PROCESSING`) is now saved before being changed and restored on every exit
path (via `atexit()`), since a child process changing shared console state
without restoring it is a separate, real way to leave a parent shell
confused; and any input the terminal sent that this process never read is
explicitly discarded on exit (`FlushConsoleInputBuffer` on Windows,
`tcflush` on POSIX) — some terminal interactions can leave response bytes
queued up that would otherwise resurface as garbage in whatever reads
input next, which is exactly the parent shell's own prompt.

### A visible sign you're "inside" npulse

Asked for explicitly — without something persistent, there's no way to
tell at a glance whether a corrupted-looking prompt (or any prompt) is a
plain shell or one npulse set up. Two layers, both automatic:

- **Window/tab title** — set to `npulse console` (the console above) or
  `npulse tracert-mtr HOST`/`npulse ifconfig (watching)` (a live view run
  directly), via `SetConsoleTitleA` on Windows or the widely-supported OSC
  0 escape sequence (`\033]0;TITLE\007`) on POSIX terminals. Restored to
  whatever it was before on Windows once a directly-run live view ends
  (no reliable "read current title" API exists on POSIX to restore from,
  but most shells with any title-setting of their own overwrite it on
  their very next prompt anyway — a minor, common, widely-tolerated
  cosmetic gap, not a functional one).
- **The console's own shell prompt** (console mode only, not a directly-run
  live view — there's no persistent prompt to modify there) — prefixed
  with `[npulse] ` for the whole session. `cmd.exe` needs only a `PROMPT`
  environment variable, which nothing overwrites afterward. bash/zsh are
  harder: an interactive shell always re-runs its own startup file
  (`~/.bashrc`/`~/.zshrc`) on launch, which unconditionally overwrites any
  inherited `PS1` before ever showing a prompt — confirmed directly by
  testing a genuinely interactive shell (a real pty on both stdin and
  stdout; piped input alone makes bash detect itself as non-interactive
  and skip this entirely, which made an earlier, more naive test of this
  look like it worked when it wasn't really being exercised). The real fix
  layers the prefix AFTER the user's own startup file runs: bash via
  `--rcfile FILE -i` (FILE sources the real `~/.bashrc` first, then
  appends the prefix last); zsh via `ZDOTDIR` pointed at a temp directory
  whose `.zshrc` does the same. Verified live: a spawned bash session
  showed `[npulse] user@host:~#` on every prompt line. Any other shell
  (plain `sh`/`dash`, fish, an unrecognized `$SHELL`) falls back to a
  plain `PS1` environment variable, which may or may not take effect
  depending on that shell's own startup behavior — a known, documented
  limitation rather than a fully solved case, harmless where it doesn't
  apply.

**Verification**: on Linux, captured the actual escape bytes emitted under
a real pseudo-terminal and confirmed the alternate-screen-buffer sequence
is correct and appears/disappears at the right moments, confirmed the
`[npulse]`-prefixed bash prompt renders correctly live, confirmed a real
`SIGTERM` mid-session (the same signal a graceful `Ctrl-C`/`timeout` sends)
is handled promptly (~3 seconds, matching the timeout given, not a hang)
with the alternate screen properly exited first. On the cross-compiled
Windows `.exe` under Wine, confirmed the same live view — report mode and
continuous mode both, including a `SIGTERM` sent mid-session — completes
correctly and promptly across multiple repeated runs. Two isolated false
alarms during this specific round of testing are worth recording plainly:
one graceful-shutdown test hung specifically when wrapped in the `script`
pty-recording utility, traced to `script`'s own known quirks in signal-
forwarding/pty-monitoring (confirmed by removing `script` from the test
and observing the exact same command complete correctly and promptly on
its own); and one single Wine run of the exact same report-mode command
that had just worked appeared to hang, which did not reproduce across
three immediate repeat runs. Both are documented here as testing-
environment artifacts, not defects in this code, precisely because
distinguishing the two matters and neither was waved away without first
trying to reproduce it in isolation.

## Standard commands (multiple familiar entry points, one binary)

`argv[0]` is checked first, so a copy or symlink of this binary named
`ping`, `tracert`, `traceroute`, `mtr`, `ifconfig`, `ipconfig`, or
`nslookup` behaves like that command directly — no subcommand needed.
Falls back to `npulse SUBCOMMAND ...` otherwise.

| You type...                              | Runs...              | Backed by |
|-------------------------------------------|----------------------|-----------|
| `ping HOST` / `npulse ping HOST`           | ICMP/UDP/TCP ping    | `PingRun` (same engine as the desktop app's Ping tool) |
| `tracert HOST` / `traceroute HOST` / `npulse tracert HOST` | One-shot progressive route trace | `Session` — classic Windows `tracert`/Linux `traceroute` output shape |
| `mtr HOST` / `tracert-mtr HOST` / `npulse tracert-mtr HOST` | Live, continuously-refreshing per-hop table | `Session` — same engine and same table the desktop app's Path/MTR view shows |
| `ifconfig` / `ipconfig` / `npulse interfaces` | Full adapter listing, `-w` to auto-refresh | `list_interfaces(true)` (same data as the desktop app's Interfaces page) |
| `nslookup HOST` / `npulse dns HOST`        | Forward/reverse DNS  | `getaddrinfo`/`getnameinfo` |
| `npulse portscan HOST -p PORTS`            | TCP connect scan     | self-contained (see main.cpp's own doc comment for why) |
| `npulse completion SHELL`                  | Shell completion script | static, per-shell |
| `npulse version` / `npulse help`           | —                    | — |

**`tracert` vs. `tracert-mtr` — two deliberately different commands, not
one with a flag.** They answer different questions: `tracert` prints the
path once and exits (like running `tracert`/`traceroute` today, at this
moment); `tracert-mtr` keeps monitoring it live (like `mtr`, or the GUI's
Path/MTR view, which has no one-shot mode at all — this CLI adds one that
the GUI doesn't have). Sharing one command with a `--live`-style flag would
have made the far more common one-shot case carry a flag it needs to
remember every time; two names, matching two real, pre-existing tools
people already know, needs none.

**Deliberately NOT installed as literal `ping`/`tracert`/`mtr` on PATH by
default.** Shadowing the OS's own tools would silently change behavior for
every other program on the system that shells out to them. Standard
install only places `npulse` itself on PATH (see "Packaging" below); copy
or symlink the installed `npulse` binary under another name yourself if you
want that, and it dispatches correctly either way — confirmed: tested by
literally copying the built binary to `/tmp/ping`, `/tmp/mtr`,
`/tmp/tracert` and running each.

## `npulse ping HOST [options]`

```
-4 / -6                  Force IPv4 / IPv6 (default: whichever the resolver
                         gives first, preferring an A record).
-c, --count N            Number of pings (default 4). Ignored with --continuous.
--continuous             Ping until Ctrl-C, ignoring --count.
-i, --interval SECS      Seconds between pings (default 1.0).
-W, --timeout SECS       Per-probe timeout (default: auto).
-s, --size BYTES         Payload size (default 56).
--ttl N                  IP TTL for the probe (default 255 — a ping measures
                         the destination directly, not an intermediate hop).
-P, --protocol icmp|udp|tcp   Probe protocol (default icmp).
-p, --port N             Destination port (udp/tcp only).
-I, --interface ADDR     Bind to a specific local interface/source address
                         (the GUI's "iface" dropdown equivalent) instead of
                         the OS's default egress choice.
```

Flag meanings are this tool's own, not a bug-for-bug match of any one OS's
native `ping` — those already disagree with each other (Windows' `-t` means
"ping until stopped"; Linux/macOS's `-t` means "set IP TTL").

```
$ npulse ping 192.0.2.1 -c 4
PING 192.0.2.1
Reply from 192.0.2.1        seq=1    time=0.18 ms
Reply from 192.0.2.1        seq=2    time=0.23 ms
Reply from 192.0.2.1        seq=3    time=0.24 ms
Reply from 192.0.2.1        seq=4    time=0.23 ms

--- 192.0.2.1 ping statistics ---
4 transmitted, 4 received, 0.0% loss
rtt min/avg/max/mdev = 0.183/0.221/0.243/0.022 ms
```

## `npulse tracert HOST [options]` (also: `traceroute`)

One-shot, progressive — prints each hop exactly once, in order, as it
settles, and stops once the destination answers or `--max-hops` is
exhausted. Matches the classic Windows `tracert`/Linux `traceroute` output
shape.

```
-4 / -6                  Force IPv4 / IPv6.
-P, --protocol icmp|udp|tcp|http   Probe protocol (default icmp).
-p, --port N             Destination port (udp/tcp/http only).
-m, --max-hops N         Maximum TTL to probe out to (default 30).
-i, --interval SECS      Seconds between probes to each hop (default 1.0).
-T, --trace-interval SECS   Route re-discovery interval — the GUI's "Trace"
                         field ("Route re-discovery interval, seconds").
                         Distinct from -i: how often the engine re-checks
                         for a ROUTE CHANGE, not how often it re-measures
                         an already-discovered hop.
-W, --timeout SECS       Per-probe timeout (default: auto).
-s, --payload BYTES      Payload size (default 56).
-I, --interface ADDR     Bind to a specific local interface/source address.
--unprivileged           Use unprivileged datagram ICMP instead of raw
                         sockets (the inverse of the GUI's "Raw" checkbox,
                         which defaults on, same as here).
```

```
$ npulse tracert 192.0.2.1
Tracing route to 192.0.2.1 over a maximum of 30 hops:

  1  192.0.2.1                                       0.23 ms

Trace complete.
```

`Session` probes concurrently across hops by design (not strictly
one-hop-at-a-time the way historic traceroute implementations do), so
"wait for real replies, then print" is adapted rather than an exact
`traceroute -q 3`-style match: a hop is considered settled once it either
resolves (an address) or goes 5 probes with zero replies (printed as
`* * *`, same as real traceroute's own silent-hop row). Documented here
rather than presented as bit-for-bit identical to any specific
implementation.

## `npulse tracert-mtr HOST [options]` (also: `mtr`)

The live, continuously-refreshing per-hop table — same engine, same fields,
as the desktop app's Path/MTR view. Takes the exact same option set as
`tracert` above (`-4/-6`, `-P/-p`, `-m`, `-i`, `-T`, `-W`, `-s`, `-I`,
`--unprivileged`), plus:

```
-c, --count N            Stop after N non-empty snapshot updates
                         (approximation, not an exact per-hop probe-round
                         count — Session's continuous discovery loop has no
                         single clean "one complete round" boundary the way
                         PingRun's sequence numbers do). Omit for
                         continuous live view until Ctrl-C.
-r, --report / --json    Static output once discovery settles, no ANSI
                         redraw — safe to pipe/redirect. Defaults to 10
                         cycles if -c isn't also given.
```

```
$ npulse tracert-mtr 192.0.2.1 -c 1 -r
npulse tracert-mtr  192.0.2.1  IPv4
------------------------------------------------------------------------------------------------
Hop  Host                                     Loss%   Sent   Recv     Last      Avg     Best    Worst
------------------------------------------------------------------------------------------------
1    192.0.2.1 [DEST]                            0.0%      1      1     0.17     0.17     0.17     0.17
------------------------------------------------------------------------------------------------
```

(bold headers/separators and a green/yellow/red loss-percentage color —
matching the GUI's own severity bands — when attached to a real terminal;
plain, uncolored text exactly as shown above the instant output is piped
or redirected. `[DEST]` marks the destination; a long hostname is
truncated with `...` rather than breaking column alignment. Deliberately
plain ASCII throughout, not Unicode — a Windows console not explicitly in
UTF-8 mode misreads multi-byte characters as several separate garbled
ones, confirmed live and fixed by removing them rather than chasing every
possible codepage/font configuration. A hop that shows `(re-resolving)`
instead of an address briefly, right after a route change, still has real
recent stats — the guarded-wipe/Frankenstein-route guard (ARCHITECTURE.md
§6) only clears a hop that was already measuring well, so recent numbers
staying good right after is expected, not a bug.)

### The Host column, and why a destination marker can never be hidden

The Host column is wider than it looks like it needs to be (46 characters)
because a real, fully-resolved IPv6 reverse-DNS hostname plus its address
routinely exceeds a narrower budget on its own — truncation there isn't a
rare edge case, it's closer to the common one. **BUG FIX, reported live**:
the `[DEST]` marker used to be appended to the hostname text *before*
truncating it, so a long enough hostname could cut the marker off
partially or entirely — hiding the one piece of information (this hop IS
the destination) that matters most, on exactly the row most likely to
have a long hostname. Fixed by truncating the hostname/address text first,
to a budget that reserves room for the marker, then appending the marker
after — it's now always fully visible regardless of how long the
underlying hostname is.

### Auto-refresh when Family is Auto (the Force-Recheck alternative)

The GUI's Force Recheck button doesn't have a CLI equivalent — it's an
interactive keypress on a live, already-running trace, and this simpler
live view doesn't have a keyboard-input reader loop (see "Not yet
implemented" below). What it has instead, specifically for `Family: Auto`
(the default): `Session::resolve()` — which decides whether IPv4/IPv6 is
actually usable — runs exactly **once**, at the very start of a trace, never
re-checked while it's running. Started before a real route came up, a trace
would otherwise show "No local IPv6 egress available" and sit there stuck
on that stale message forever, with no way to retry short of restarting the
whole command by hand.

`tracert-mtr` self-heals this automatically: if a session under `Family:
Auto` reports an error continuously for more than 15 seconds, it's torn
down and rebuilt from scratch (re-running family detection), and this
repeats every few seconds until it succeeds or you press Ctrl-C. A pinned
`-4`/`-6` never does this — you asked for that exact family, and an
unavailable one should keep saying so, not silently swap families on you.

Verified live: run under a genuinely IPv6-less environment with
`Family: Auto` against a literal IPv6 address — retries every ~3s exactly
as designed; a matching `-6`-pinned run shows the error once and never
retries; Ctrl-C stops the retry loop immediately either way (confirmed
under ASan/UBSan with no thread-safety issues from the internal poller
thread this uses to combine "real Ctrl-C" and "internal restart decision"
into the one stop signal `Session::run()` accepts).

**Not to be confused with:** the engine also has an unrelated, passive
background mechanism (sometimes also called "auto refresh" informally)
that re-verifies an already-resolved hop's address after a silent route
change, as an alternative to the GUI's manual Force Recheck button — see
`ARCHITECTURE.md` §6, "Why 'auto refresh' can look like it does nothing,
right after Force Recheck visibly works", for what that one does and why
it's much slower by design (a real ~60+ second minimum with the 30s
default — configurable from the GUI's Settings window, see
`set_default_recheck_tuning`, session.hpp) than pressing Force Recheck.
The family self-heal described above is a different, CLI-`tracert-mtr`-
specific feature: it retries family *detection*, not hop *addresses*, and
has no Settings-window equivalent.

## `npulse ifconfig` (also: `ipconfig`, `npulse interfaces`)

Lists every adapter — up or down, including loopback — with address,
family, status, type, MTU, and the same `usable`-egress classification
(`is_cacheable_ip()`) the engine itself checks before probing:

```
$ npulse ifconfig
npulse ifconfig
------------------------------------------------------------------------
ADAPTER        ADDRESS                  FAM  STATE  TYPE     MTU    EGRESS
------------------------------------------------------------------------
lo             127.0.0.1                v4   up     loop     65536  not usable
eth0           192.0.2.2                v4   up     normal   1400   usable
------------------------------------------------------------------------
```

```
-w, --watch [SECS]    Auto-refresh (default interval: 5s if given with no
                      number). Clears and redraws on each cycle — useful
                      for watching an adapter come up/go down live (a cable
                      unplugged/replugged, a VPN connecting) the same way
                      the GUI's own interfaces view would reflect it, since
                      a one-shot CLI process otherwise has no way to notice
                      a later change on its own.
```

## `npulse dns HOST_OR_IP` (also: `nslookup`)

Forward lookup (every A/AAAA record) for a hostname; reverse (PTR) lookup
for a literal IP.

## `npulse portscan HOST -p PORTS`

```
-p, --ports SPEC     Comma list (80,443,8080) or a range (1-1024).
-W, --timeout SECS   Per-port connect timeout (default 1.0).
```

## Shell auto-completion (typing suggestions)

`npulse completion SHELL` prints a completion script for bash, zsh, fish,
or powershell — the same `kubectl completion`/`docker completion` pattern.
Completes subcommands (`ping`, `tracert`, `tracert-mtr`, `ifconfig`, `dns`,
`portscan`, `completion`), the full current flag set (including `-T`, `-I`,
`--unprivileged`, `-w`/`--watch`), and — bash/zsh only, via a shared
recent-targets file at `~/.local/share/netpulse/cli-recent` — recently-used
hosts. **Not yet wired up**: the file path is referenced in the generated
scripts, but nothing in `main.cpp` writes to it yet.

```sh
echo 'source <(npulse completion bash)' >> ~/.bashrc
echo 'source <(npulse completion zsh)' >> ~/.zshrc
npulse completion fish > ~/.config/fish/completions/npulse.fish
Add-Content $PROFILE 'npulse completion powershell | Out-String | Invoke-Expression'
```

## Terminal compatibility (Windows Terminal / PowerShell 7, and everywhere else)

**BUG FIX, reported against a real Windows run** (`tracert-mtr` on classic
"Windows PowerShell" — `powershell.exe`, not `pwsh.exe`): every refresh
printed a brand-new copy of the table below the last one instead of
updating in place. Root cause: a Windows console only interprets ANSI/VT
escape sequences from a process's own output once that process explicitly
opts in via `SetConsoleMode(..., ENABLE_VIRTUAL_TERMINAL_PROCESSING)` on
its own stdout handle — this is Microsoft's documented mechanism, not
automatic, and classic `powershell.exe` (unlike Windows Terminal or
PowerShell 7/`pwsh.exe`, which both enable it themselves) doesn't do this
for a child process by default. Without it, the escape codes this tool was
already emitting were just inert bytes — no clear, no cursor move — so each
redraw's output simply appended below the previous one, exactly the
scrolling behavior reported.

Fixed with the real mechanism, not a workaround: `term_init()` (run once,
at startup) explicitly requests `ENABLE_VIRTUAL_TERMINAL_PROCESSING` on
Windows and checks the terminal is a real console (not a pipe/redirect,
via `isatty`) before enabling any ANSI feature at all — plain, uncolored,
uncontrolled output otherwise, so a non-interactive run (piped, redirected,
CI) is never at risk of stray escape bytes regardless of platform. On top
of that, `tracert-mtr`/`ifconfig -w` no longer even use a full-screen clear
(`\033[2J\033[H`) every frame — that was ALSO wrong on its own terms,
independent of the VT-mode bug: blanking the whole screen before redrawing
is what causes visible flicker on every single refresh, which is not what
`mtr`/`htop`/SolarWinds Traceroute NG actually do. The live views now move
the cursor back to the top and overwrite in place (clearing only a
trailing leftover line or a shorter final frame), which is genuinely
flicker-free — confirmed by capturing the raw escape sequences emitted
under a real pseudo-terminal (Linux `script`) and reading them back
directly rather than only visually: `\033[H` once per frame, `\033[K`
after each line (eats a longer previous line's leftover characters), and
`\033[J` once at the very end (eats any leftover trailing rows from a
frame that had more of them).

`-r`/`--report`/`--json` output never includes any of this — no redraw
codes at all, even if run attached to a real interactive terminal rather
than piped, so it stays genuinely safe to pipe/redirect/diff. Color
(loss-percentage severity, bold headers, dimmed secondary text) is
separate from the redraw machinery and still applies in `--report` mode
when attached to a real terminal, exactly like `ls --color=auto`, `git
status`, and most modern CLI tools — and is automatically and completely
suppressed the instant output isn't a real terminal (piped, redirected),
confirmed directly by comparing captured output with and without a
pseudo-terminal attached.

## Exit codes

```
0   Success.
1   Usage error, or unknown subcommand.
```

`ping` returns 1 if every probe timed out. `tracert` returns 1 if it
reached `--max-hops` without confirming the destination. Everything else
returns 0 on completion regardless of individual probe/hop outcomes.

## Permissions

Raw ICMP (used for TTL-expiry discovery in `tracert`/`tracert-mtr` under
all four protocols, and for `ping`'s own ICMP mode, unless `--unprivileged`
is given) needs elevated privileges on every supported OS — same
requirement the desktop app has, documented in `SECURITY.md`:

```
Linux     Run as root, OR grant the capability once:
              sudo setcap cap_net_raw+ep $(which npulse)
Windows   Run from an elevated ("Run as administrator") terminal.
macOS     Run as root (sudo).
```

`npulse ifconfig`, `npulse dns`, and `npulse portscan` need no elevated
privileges at all.

## Not yet implemented (known, honest gaps)

- **Interactive keyboard shortcuts** for `tracert-mtr`'s live view (the GUI
  has per-hop pause, a manual Force Recheck key, live interval adjustment)
  — this version has no keyboard-input reader loop at all; Ctrl-C is the
  only interactive control right now. `Family: Auto`'s auto-refresh above
  covers the one Force-Recheck-shaped gap that could be solved without an
  interactive loop; the rest (pausing a specific hop, live interval tweaks)
  still need one.
- **Shell completion's recent-hosts file** isn't written yet (see above).
- **Per-hop pause** (`Settings::paused_hops`, a real field the engine
  already supports) has no CLI flag yet — not meaningful for `tracert`'s
  one-shot mode, but could be a `--pause-hop N` flag on `tracert-mtr`.

## Packaging: bundled into every OS installer, not a separate download

Built as a **separate executable target** (`cli/`, CMake target `npulse`)
from the same top-level `CMakeLists.txt` the Tauri app's native C++ engine
also builds from — `NETPULSE_BUILD_CLI` (default ON) means a plain `cmake
--build .` from a source checkout produces both `netpulse_tests` and
`npulse` with no extra flags.

Bundled via Tauri's `externalBin` sidecar mechanism
(`tauri.conf.json`/`bundle.externalBin`) into every installer, plus
published as its own standalone per-OS download on the GitHub Release
(`npulse-<os>[.exe]`) — see `README.md`'s Development section and
`tauri-app/src-tauri/binaries/README.md` for the build commands (verified
end-to-end on the machine this was built on) and `ARCHITECTURE.md` §11 for
the full packaging breakdown per OS, including which parts (Windows NSIS/
EnVar PATH registration, in particular) are written to match documented
conventions but not yet verified against a real install.

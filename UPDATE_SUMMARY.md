# Session summary: hop-14 fixed, real Windows UDP bug found and fixed, HTTP actually implemented, port-collision bug found and fixed

## Follow-up: HTTP confirmed working on your real machine, UDP still stuck

Your screenshot showed `HTTP:80` on `1.1.1.1` with real, working data
(`avg 276.9ms`, `pl 0%`) — confirming the HTTP implementation genuinely
works on your actual Windows hardware, and that you're running the
latest build. `UDP:53` on the same `1.1.1.1` was still stuck at
DISCOVERING with every hop at zero, meaning the `SIO_UDP_CONNRESET` fix
from last session wasn't sufficient on its own.

## Found a second, real, independent bug: port collision across sessions

Both `ProbeTcp::new_flow_pin()` and `ProbeUdp::new_flow_pin()` derived
their source port purely from `reinterpret_cast<uintptr_t>(this)` — the
object's own memory address — with **zero coordination across different
session instances**. Your screenshot showed exactly three simultaneous
sessions of three different protocols (TCP:443, HTTP:80, UDP:53) — and
heap allocations for same-sized small objects (exactly what
`ProbeTcp`/`ProbeUdp`/`ProbeHttp` instances are) frequently land in
predictable, closely-clustered address ranges, especially when several
targets get added in quick succession.

The reply-routing registry (`register_icmp_owner`, a plain
`map<port, Session*>`) has **zero collision tolerance**: whichever
session registers a given port *last* simply wins, and the other
session's replies are silently discarded, forever, with no error. A stale
comment on `ProbeTcp::new_flow_pin()` justified the address-based
derivation by citing the shared-hop *adoption* cache's collision
tolerance (`ARCHITECTURE.md` §4) — but that's a completely different
mechanism from the reply-routing registry, and the reasoning didn't
actually apply here.

**Fixed**: replaced the address-based derivation in both `ProbeTcp` and
`ProbeUdp` with a shared, process-wide, atomically-allocated port-block
allocator (`allocate_flow_port_block()`, `probe.hpp`) — each session now
gets a guaranteed-distinct 256-port range, no matter how many sessions of
whatever protocol mix are running simultaneously.

**Honest caveat**: I verified this compiles cleanly and re-ran a live
3-session test (TCP + HTTP + UDP simultaneously, matching your exact
scenario) confirming UDP still received real hop data under load — but
UDP was already partially working in my sandbox's own testing both before
and after this specific change, so I can't claim this sandbox test proves
it was *the* cause of your specific report the way earlier live-tested
fixes could. It's a real, confirmed bug worth fixing regardless of
whether it's the full explanation for what you're seeing.

## Added real diagnostics, in case this still isn't enough

If UDP is still stuck after this fix, guessing further without data isn't
productive. Added:
- `ProbeUdp::last_send_errno()` — captures the actual OS socket error
  code (`WSAGetLastError()` on Windows) from the most recent failed send.
- `run_udp()` now logs `[netpulse-udp] send FAILED at ttl=N errno=X` via
  stderr when a send fails, gated behind the existing `NETPULSE_DEBUG`
  environment variable.

**If UDP is still not working after this update**: run the app with
`NETPULSE_DEBUG=1` set (from the same terminal you already use for
`npx tauri dev`) and check the terminal output for `[netpulse-udp]`
lines — that will show the actual Windows error code for any failed
send, which is real data I can act on directly instead of continuing to
guess at Windows-specific socket behaviors from a Linux sandbox.

## Verification this session

- Full CMake build x2 configurations (native, Windows cross-compile) —
  both clean, native passes the test suite, Windows produces a valid
  `PE32+` executable.
- Live 3-session test (TCP + HTTP + UDP simultaneously) — UDP received
  real hop data under the same multi-protocol load as your screenshot.


## Hop-14 — real bug, fixed

Same root cause identified last time, now also fixed for the case where a
hop entry lingers *beyond* the confirmed destination. Both `run_tcp()` and
`run_udp()` now prune any hop entry past `dest_hop_` once it's known,
matching what the ICMP loop has always done. A TTL that also happened to
reach the destination (but wasn't the smallest such TTL, which
`dest_hop_` always prefers) no longer sits there forever with frozen,
misleading data.

## UDP stuck at DISCOVERING — found the actual root cause

This is a specific, well-documented Windows networking quirk, not a
generic "something's broken" — which is exactly why it never reproduced
in this sandbox's Linux testing.

**The mechanism**: Windows propagates an ICMP error triggered by a UDP
send back to that *same socket's* next call (send or receive), which then
fails with `WSAECONNRESET`. This is a long-standing, documented Windows
behavior with no equivalent on POSIX. UDP-traceroute's entire mechanism
is *deliberately* eliciting exactly this kind of ICMP error — Port
Unreachable from the destination once a probe reaches it. Since one
persistent UDP socket serves every probe a session ever sends, the very
first time the mechanism successfully worked would poison that same
socket, silently failing every subsequent send — explaining precisely
what you saw: works initially (or not even that, if an intermediate
router's reply arrived first), then permanently and completely stuck,
independent of which port was chosen.

**Why TCP wasn't affected**: this quirk is specific to UDP sockets
receiving ICMP errors; TCP's connection-oriented model doesn't have the
equivalent failure mode, which is exactly why TCP mode kept working
throughout while UDP didn't — a real, useful diagnostic signal I used to
narrow this down.

**Fixed**: `SIO_UDP_CONNRESET` ioctl, set immediately after socket
creation, disabling this propagation — the standard, officially
documented fix for this exact problem. No-op on POSIX (guarded by
`#ifdef _WIN32`), verified to compile correctly for the actual Windows
target via cross-compile. I can't get live Windows confirmation from this
sandbox, but this is the specific, correct, standard fix for the specific
symptom you described, not a guess.

## HTTP — actually implemented this time, verified against a real server

Built `ProbeHttp` (`probe_http.hpp`/`.cpp`): reuses `ProbeTcp` entirely for
hop-discovery (an intermediate router doesn't run an HTTP server, so
there's nothing HTTP-specific needed there), and adds a genuine
connect → send HTTP request → wait for a real response cycle for the
destination — a materially stronger check than TCP mode's "the port is
open," which is what makes this actually HTTP rather than a relabeled
TCP probe.

**Verified live, not just compiled** — twice, catching a false positive
along the way:
1. First test appeared to succeed, but inspecting the raw response bytes
   showed it was actually my own sandbox's egress proxy rejecting the
   connection (`x-deny-reason: host_not_allowed`), not a real server
   response. Caught this before treating it as verification.
2. Retested against `github.com`'s real IP (an allowed domain from this
   sandbox) — got back a genuine `301 Moved Permanently` with GitHub's
   actual redirect-to-HTTPS response, confirmed via raw byte inspection.
3. Ran a full `Session` in HTTP mode end-to-end against the same real
   server: correctly identified the destination, `sent`/`recv` climbing
   consistently, stable ~100ms RTT across multiple real request/response
   round trips, zero negative RTT or duplicate destination flags (the
   earlier TCP fixes apply here too, since HTTP mode reuses the same
   consolidation logic).

**Wired through the full chain**: FFI (`netpulse_ffi.cpp`'s protocol
parsing), `manager.hpp`'s two JSON serialization sites, and the frontend
dropdown — HTTP is now genuinely selectable and functional, not
disabled/placeholder.

**Also fixed the same build-script gap as last time**: `probe_http.cpp`
was missing from `build.rs`'s explicit source list (the Tauri/cxx build
path, separate from the CMake glob that would have picked it up
automatically) — caught this before it repeated the exact linker error
from the TCP/UDP rollout.

**Scope, stated plainly**: plain HTTP only, port 80, no TLS. HTTPS needs a
real TLS library integration — genuinely separate work, not attempted
here. `HEAD` requests only, minimal response handling (any response bytes
count as success; no status code parsing or redirect following yet).

## QUIC — still not implemented

Unchanged from before: needs an actual QUIC library (msquic/quiche/
ngtcp2) and its own connection state machine. Still shown disabled in the
UI with an honest tooltip, not silently missing.

## Verification this session

- Full CMake build x3 configurations (native, obfuscated, Windows
  cross-compile via MinGW-w64) — all clean, native + obfuscated pass the
  test suite, Windows produces a valid `PE32+` executable, with
  `probe_http.cpp` correctly included in all three.
- **Live HTTP verification against a real server** (GitHub), including
  catching and correcting a false-positive test result along the way.
- Live `Session`-level test in HTTP mode, checked against the same
  negative-RTT and duplicate-destination-flag bugs found and fixed
  earlier in TCP/UDP — confirmed absent here too.
- Full `npm install` (0 vulnerabilities) + `npm run build` with
  obfuscation on the final state.
- `esbuild` parse check on `App.jsx`.

## What's still open

- Confirm the `SIO_UDP_CONNRESET` fix resolves UDP on your actual Windows
  machine — this is the one fix in this session I couldn't get live
  confirmation of from this sandbox.
- HTTPS (TLS) — separate, substantial undertaking.
- QUIC — separate, substantial undertaking.
- HTTP response handling could grow (status codes, redirect following,
  GET vs HEAD) as a future refinement — current scope is deliberately
  minimal but real.

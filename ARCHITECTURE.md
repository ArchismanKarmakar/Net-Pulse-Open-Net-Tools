# Architecture

This document describes **how the engine actually works** — the design intent,
the low-level mechanics, and the threading model — for anyone modifying
`core/`. It intentionally goes deeper than the README, which documents
user-facing behaviour.

Net Pulse's engine solves one problem: run **continuous, concurrent,
per-hop path monitoring** (traceroute + ping, fused) for many targets at once,
without ever looking like a flood/scan to a router, an ISP's control plane, or
host antivirus/behavioural heuristics — while staying correct across route
changes, routing loops, interface changes, and outages.

---

## 1. Process shape

```
Tauri webview (React UI, tauri-app/dist)
   │  invoke('add_target', …)  — Tauri IPC, in-process
   ▼
tauri-app/src-tauri  (Rust host, commands.rs → ffi.rs)
   │  cxx bridge — owns a netpulse::Manager, one thread per target session
   ▼
core/  (this document)
   ├── Session::run()        — one thread PER TARGET (owns per-target state only)
   ├── RxDispatcher           — ONE thread, process-lifetime (reads all sockets)
   ├── RdnsResolver           — 6 threads, process-lifetime (PTR lookups)
   └── GlobalPacer / SharedHopTable / SocketPool — shared state, no owning thread
```

Nothing here opens a TCP/HTTP port. The only sockets are raw/datagram ICMP
sockets used for probing. `Manager` (the Rust/`cxx` FFI layer) is out of scope
for this document except where it calls into `Session`.

### Threads, precisely

| Thread | Count | Lifetime | Owns |
|---|---|---|---|
| `Session::run()` | 1 per active target | until target removed/stopped | `hops_`, `pending`, per-target pacing, discovery/loop-auditor state |
| `RxDispatcher::run()` | 1 (process) | first target start → process exit | nothing but a `select()` loop; never touches `Session` state directly |
| `RdnsResolver::worker()` | 6 (process) | first target start → process exit | nothing but the shared PTR cache/queue |
| (pool) `Prober` | 1 per distinct `(family, privileged, source_addr)` combo | ref-counted; dies when last owner releases it | one raw/datagram socket |

Every "process-lifetime" thread above is **detached**, and every singleton it
touches is **heap-allocated and never destructed** (`static T* p = new T();
return *p;`). This is deliberate, not sloppy: C++ static-destruction order
across translation units is unspecified, but these threads keep running for as
long as any `Session` thread might touch them — including during whatever
order the process happens to tear things down in. A `new`'d-and-leaked
singleton simply never has a destruction race to have. The OS reclaims the
memory at process exit regardless.

---

## 2. Why a socket pool + one shared RX dispatcher (Pillar 1)

**Naïve design** (what a first-cut traceroute tool does): each target owns its
own raw socket, sends on it, and blocks in `recvfrom()` on that same socket.

**Why that breaks at scale, concretely:**
1. **N sockets × N targets on the same address family/interface** — the OS
   sees N raw ICMP sockets all bound the same way. On Windows in particular,
   an inbound ICMP reply is not guaranteed to be delivered to *every* raw
   socket that could accept it — it can land on just one. If that one belongs
   to target A but the reply's ICMP id says it's actually for target B, B's
   socket never sees it: it looks like 100% loss for B on a hop that answered
   fine. This was the original bug behind "one target's traffic causes packet
   loss on another's shared hop" — it wasn't rate-limiting *at all*, it was
   reply misdelivery.
2. **File-descriptor and OS-object growth** is unbounded in the number of
   targets, for no benefit — most targets share the exact same
   `(family, privileged, source_addr)` triple (default route, raw mode, auto
   family).

**Fix:** collapse socket ownership to **one `Prober` per distinct
`(family, privileged, source_addr)` combination**, shared (ref-counted) by
every `Session` that needs that exact combination — typically 1–2 sockets for
the whole process, but a genuinely multi-homed setup (different targets bound
to different NICs, or mixing raw/unprivileged) still gets its own socket per
distinct combo, so that feature is preserved exactly.

```cpp
// transport.hpp
std::shared_ptr<Prober> acquire_pooled_socket(Family family, bool privileged,
                                               const std::string& source);
```

`SocketPool` (`transport.cpp`) is a `map<SocketPoolKey, weak_ptr<Prober>>`
under a mutex. `acquire()` locks, looks up the key, and either returns the
existing socket (`weak_ptr::lock()` succeeded) or constructs a fresh one and
stores a new `weak_ptr`. A dying entry (last `shared_ptr` owner dropped) is
**lazily replaced** by the next `acquire()` for that key rather than eagerly
erased on last release — eager erase would race a concurrent `acquire()` for
the same key (the dying entry's cleanup could erase a brand-new entry inserted
moments earlier); overwriting a stale/expired `weak_ptr` under the same lock
is always safe.

### TX correctness: `send_mtx_`

A pooled socket is now sent on by *multiple threads concurrently*.
`sendto()` itself is safe to call concurrently (each call is one atomic
datagram write) — but **TTL is not a per-packet parameter**. `IP_TTL` /
`IPV6_UNICAST_HOPS` is *socket-level* state set via `setsockopt()`
immediately before `sendto()`. Two threads racing `{set TTL A} {set TTL B}
{both send}` can produce a packet that goes out with the **wrong** TTL —
silently corrupting exactly the measurement the hop count depends on.

`Prober::send()` wraps `{setsockopt(TTL), sendto()}` in a `std::lock_guard`
on a per-`Prober` `send_mtx_`. This is a small, fast critical section —
negligible contention even at the pacer's real send rate (tens/sec, not
millions) — deliberately chosen over a lock-free MPSC queue + dedicated TX
thread: same correctness guarantee, far less new machinery to get subtly
wrong.

### RX: one dispatcher, not N blocking reads

Nothing calls `recvfrom()` from inside `Session::run()` anymore.
`Prober::drain_ready()` is a **non-blocking** drain of whatever is currently
queued on that one socket. The only caller is `RxDispatcher`:

```cpp
// session.cpp, RxDispatcher::run() — one thread for the whole process
for (;;) {
    auto socks = list_active_pooled_sockets();     // fresh snapshot every iteration
    select(...);                                    // across every pooled socket's fd, 100ms timeout
    for each readable socket:
        for (auto& inc : sock->drain_ready())
            Session::dispatch_incoming(inc);         // routes by ICMP id, see below
}
```

The 100ms `select()` timeout (rather than an unbounded wait) exists so a
**brand-new** pooled socket — e.g. the first target to use a not-yet-seen
interface — gets picked up on the very next iteration, at most ~100ms of
extra latency on its first reply only.

### Cross-session routing: the registry

Every `Session` has a per-session-unique `icmp_id_` and registers itself in a
process-global `map<uint16_t /*icmp id*/, IcmpOwner*> g_registry` for the
duration of its `run()`. `dispatch_incoming()` (called only by the RX
dispatcher) looks a reply up by the ICMP id embedded in the parsed ICMP
packet, and pushes it straight into *that* owner's `inbox_` — a
mutex-protected `deque<Incoming>` — then notifies `inbox_cv_`.

`IcmpOwner` is a small abstract interface (`session.hpp`) with one method,
`push_incoming(const Incoming&)`; `Session` implements it, and so does
`PingRun` (`ping_run.hpp` — the standalone Ping tool's engine, §12's file
map). The registry itself only ever deals in `IcmpOwner*`, not `Session*` —
that's what lets the Ping tool share this exact socket pool and dispatcher
rather than needing its own thread or its own registry.

This removes the entire class of "which socket did the OS hand it to" bug
from part 1: there's no more ambiguity about socket ownership because replies
are demultiplexed by ICMP id at the dispatcher, not by which socket happened
to receive them.

`Session::run()`'s own loop never touches a socket for reading; it blocks on
`inbox_cv_.wait_for(lk, slice, predicate)` — the same low-jitter,
interrupt-driven wake-up property a direct `select()` on its own socket used
to have, just now serviced by the shared dispatcher instead.

---

## 3. Why direct-echo measurement, not pure TTL-limited probing (Pillar 2)

Traceroute (and old NetPulse) measures hop *h* by sending an Echo Request to
the **destination** with **TTL = h**, forcing hop *h* to decrement TTL to 0
and emit an ICMP **Time-Exceeded** back. This is how you *discover* the hop.
But measuring the hop's *health* this way is structurally wrong:

- Generating a Time-Exceeded is **control-plane** work on a router — the CPU
  path, not the fast hardware-forwarding path — and every router
  rate-limits it hard (often a few per second) regardless of how healthy the
  link actually is.
- The result: a perfectly healthy intermediate hop (one that answers a
  standalone `ping <hop-ip>` at 0% loss) reads 30–100% loss purely from
  Time-Exceeded rate-limiting, while the final destination — reached with a
  full-TTL Echo, answered with an **Echo Reply** (data-plane, not
  rate-limited the same way) — reads a clean 0%. This is the textbook
  "loss at an intermediate hop, none at the end" traceroute artifact, and it
  gets *worse*, not better, as more targets share that hop — because more
  targets means more Time-Exceeded-eliciting probes hitting the same
  rate limiter.

**Fix:** once a hop's IP is *known* (discovered via one TTL-limited probe),
stop asking it to generate Time-Exceeded and instead **ping that hop's own IP
directly**, full TTL (`kDirectEchoTtl = 255`), exactly what `ping <hop-ip>`
does. Its Echo Reply is not subject to the same control-plane rate limit, so
an echo-responsive hop reads ~0% loss whether 1 target or 100 are running.

```cpp
bool use_direct = have_addr && !echo_silent.count(ttl) && !due_recheck && !loop_audit;
sock->send(hip, kDirectEchoTtl, icmp_id_, seq, payload, pin);   // direct
// vs.
sock->send(*dest_, ttl, icmp_id_, seq, payload, pin);            // legacy/TTL-limited
```

**A direct-echo reply is only accepted from the exact IP pinged** — a
mismatched source proves nothing either way, so it's left pending to either
get a valid reply or expire via the timeout path.

**Echo-silent fallback.** Some hosts genuinely never answer an unsolicited
Echo Request (strict ICMP policy), even though they happily generate
Time-Exceeded. After `kEchoTestTries` (4) consecutive direct-echo misses with
no direct reply *ever* received, that hop is marked `echo_silent` and falls
back to legacy TTL-limited probing permanently — its rate-limit loss is then
unavoidable, exactly what `mtr`/`tracert` would show for it. This is a
one-way fallback per hop-identity; a route flap (different device answers)
clears it (see §6).

**Periodic re-confirmation (`hop_recheck_secs()`, 30s by default).** Once a
hop is on direct-echo, its TTL-elicited identity is no longer probed every
round — so a silent mid-session reroute (a different device now answers at
this position) would go unnoticed forever, since we'd just keep pinging the
*old* device's IP. Every `hop_recheck_secs()` (a process-wide runtime
default — 30s out of the box, changeable from the GUI's Settings window
without a rebuild; see `set_default_recheck_tuning`, session.hpp, and §6's
own note below), one legacy TTL-limited probe is sent instead, re-confirming
(or updating) the hop's real address — deliberately rare (well under 3
probes/sec even summed across 100 targets sharing a hop) and, unlike the
interval measurement, **never satisfied by shared-hop adoption** (§4) since
its whole point is independent re-verification.

---

## 4. Why a cross-target shared-hop cache, keyed on the edge (Pillar 4)

The global pacer (§7) bounds the *aggregate* send rate, but the more honest
fix for a hop that many targets share (home router, ISP BNG, common upstream)
is to **stop probing it redundantly at all**. All targets on the same egress
traverse the exact same early hops — there's no reason for 100 targets to
each ping the router once per interval when one probe answers the question
for all of them.

### Publish/subscribe

Whichever session is already probing a given responder IP **publishes** its
latest real reply to a process-global table; every other session **adopts**
that measurement instead of sending its own probe for that hop this round.
The router then sees ~one probe per interval total, not one per target — this
is the actual fix for "loss at a shared hop that scales with target count,"
not just a rate cap on top of the problem.

```cpp
// session.hpp
struct SharedSample {
    double ts; double rtt;
    uint64_t owner_session_id;   // publishing Session::id_
    uint8_t hop; std::string predecessor;  // attribution, for debugging
};
struct SharedHopTable {
    std::shared_mutex mtx;                          // many concurrent adopters never block each other
    std::unordered_map<std::string, SharedSample> map;
};
```

`shared_publish_to()` takes a `unique_lock` (write); `shared_adopt_from()`
takes a `shared_lock` (read) — a read-mostly table under many concurrent
adopters never serializes them against each other.

### Gate 1 — public IPs always; private IPs when a real source disambiguates them

A **private/CGNAT** address (`192.168.x`, `10.x`, `100.64/10`, link-local,
ULA, …) is only unambiguous **within one routing domain**. The exact same
private IP can be two entirely different physical devices behind different
VRFs/NAT boundaries — a target reaching *its own* home router at
`192.168.1.1` and a different target reaching a *different* router at the
same address on a different network segment are not the same device, and
blindly sharing one's RTT for the other would silently attribute a
measurement to the wrong hardware. A **public** IP has no such ambiguity — it
is globally unique by the internet's own addressing invariant — so
cross-target sharing of a public hop's measurement is always safe in
principle, regardless of `source`.

`is_public_ip()` (`session.cpp`, external linkage, mirrors
`web/src/bgp.js`'s `isPublicIp()` exactly — same exclusion ranges: `10/8`,
`127/8`, `169.254/16`, `172.16–31/12`, `192.168/16`, CGNAT `100.64/10`, IPv6
`fe80::/10`, `fc00::/7`) still classifies public vs. private exactly as
before. What changed is what happens to the private case.

**BUG FIX — private hops used to be excluded from the cache outright,
regardless of `source`.** That was overly conservative: the cache key
already includes a `source` component (`(source_addr, predecessor,
responder_ip)`), and `source_addr` genuinely does disambiguate two different
routing domains *when it's set* — the ambiguity only exists because most
targets never explicitly bind an egress interface, leaving `source_addr`
empty for the common case, which collapses every session onto the same key
component regardless of which interface the OS actually routes them
through.

The fix keeps public IPs cacheable unconditionally, and makes private/CGNAT
IPs cacheable **too, but only alongside a non-empty `source`** — see
`is_cacheable_ip()` (rejects only genuinely meaningless values: empty/`"*"`,
loopback, unspecified, link-local — link-local specifically can't be
disambiguated by `source` at all, since it's identical on every interface by
definition) and `cache_gate(ip, source)` (the actual combined rule) in
`session.cpp`. The `source` fed into `shared_publish`/`shared_adopt` is now
`Session::effective_cache_source(configured_source)`: the session's
explicit `source_addr` when set, else `local_egress_` if `resolve()` managed
to determine one, else empty (falls back to the original public-only
behavior for that session — safe by construction, never wrong, just
conservative).

`local_egress_` (`local_egress_ip()`, `transport.cpp`) is the standard "UDP
`connect()` trick": open a throwaway `SOCK_DGRAM` socket, `connect()` it to
the real resolved destination (this **never sends a packet** — UDP
`connect()` only consults the routing table and records which local address
it would use), read that decision back via `getsockname()`, close the
socket. That's the OS's own routing table answering "which interface would
traffic to this destination actually leave from" — the same decision the
real probes get — so two sessions whose destinations route out different
interfaces (physical NIC vs. VPN adapter, two default gateways, …) get
different, correctly-disambiguating keys with zero user configuration,
while sessions that genuinely share an egress (the common, valuable case)
correctly collapse onto one cache entry — including for their private hops
now, not just their public ones. Private hops (including the user's own
router) were always *measured* either way; the only thing that changed is
whether other targets on the same real egress can now share that
measurement instead of each probing the same router redundantly, exactly
like public hops already could.

### Gate 2 — key on the *edge*, not just the node

A flat `(source_addr, responder_ip)` key still conflates two conceptually
different situations even after gating to public IPs:

- **"The same physical router, reached the same way"** — safe, and the
  common, valuable case (every target sharing an egress converges on the same
  early hops).
- **"The same public IP is visible from two genuinely different upstream
  path segments"** — e.g. two regional edges converging on a shared core
  router, or real ECMP path variance (see §5) — where blindly sharing would
  silently overwrite one path's real number with the other's.

**Fix — key on `(source_addr, predecessor, responder_ip)`**, where
`predecessor` is the address of the hop *immediately before* this one **on
the publishing session's own current path**:

```cpp
inline std::string shared_key(const std::string& src, const std::string& predecessor,
                               const std::string& ip) {
    return src + '\x1f' + predecessor + '\x1f' + ip;
}
```

Two targets that share the same predecessor→responder edge (the overwhelming
common case) still collapse onto one cache entry and get the full sharing
win. Two targets that reach the *same* public IP via *different* predecessors
now get **separate** entries, so a genuine fast/slow difference between the
two paths is preserved as real data instead of one path's number silently
overwriting the other's.

**Finding the predecessor.** The naive approach — `hops_[hop-1].address()` —
looks fragile the moment hop-1 doesn't answer a TTL-limited probe (many real
routers silently forward without ever generating Time-Exceeded, while still
forwarding fine), which might suggest falling back to the nearest *already-
resolved* hop further back as a stand-in. **That fallback is deliberately
NOT used**, for the same reason the edge-keying fix above exists in the
first place: using the nearest resolved hop as a stand-in collapses every
target whose immediate predecessor happens to be unresolved onto whichever
earlier hop DID resolve, even though two targets can genuinely diverge at
that unresolved hop and reach different next hops — they'd wrongly share one
cache entry and stomp each other's real measurements. Instead,
`predecessor_of()` looks at hop-1 only, and if it hasn't resolved, returns a
sentinel keyed to that specific missing depth (`"UNKN-<h>"`), not the shared
`"SRC"` sentinel reserved for hop 1 itself:

```cpp
auto predecessor_of = [&](uint8_t hop) -> std::string {
    if (hop <= 1) return shared_hop_src_sentinel();   // "SRC" — this IS hop 1
    uint8_t h = hop - 1;
    auto it = hops_.find(h);
    if (it != hops_.end() && it->second.address()) return *it->second.address();
    return "UNKN-" + std::to_string(h);               // distinct per unresolved depth
};
```

Per-depth sentinels mean two targets that both have an unresolved hop-1 (for
unrelated reasons, or because they've genuinely diverged before hop 1
resolves) don't collide either — they get `"UNKN-1"` each, as part of a key
that also includes their own `responder_ip`, which differs if they've truly
diverged.

**BUG FIX (protocol parity):** this exact logic lives in **four** places —
one `predecessor_of` per probe loop (ICMP, UDP, TCP, HTTP; see
`Session::run()`/`run_udp()`/`run_tcp()`/`run_http()`, `session.cpp`) — since
`SharedHopTable` is keyed identically regardless of which protocol
discovered the hop. The ICMP copy had this fix; the other three had NOT been
updated to match and still used the walk-back-to-nearest-resolved-hop
version this section argues against, meaning UDP/TCP/HTTP-mode targets were
exposed to exactly the cross-contamination hazard ICMP-mode targets were
already protected from. All four are now identical.

### Attribution and ownership safety

Every entry carries `owner_session_id` (`Session::id_`, a monotonically
increasing `uint64_t` from the manager, never reused for the process
lifetime) plus `hop`/`predecessor` for debuggability — a future debug surface
can show exactly which target/hop/path a cached sample came from. This
deliberately replaced an earlier design that compared identity via a raw
`const void*` (`this`) — never dereferenced, so not unsafe, but fragile: if
session A is destroyed and a later session C's heap allocation reuses A's old
address, a stale table entry could make C misidentify itself as A's own
entry's owner (harmless in practice, but avoidable). `id_` has no such
lifetime hazard.

### Freshness and self-healing

Adoption only uses a sample fresher than `interval * 1.5 + 0.05` seconds and
never adopts the owner's own entry (`owner_session_id == self_session_id` →
always probe for real). If the owning session's hop starts actually dropping,
it simply stops publishing fresh samples — the entry goes stale within about
one interval, every adopter falls back to real probing on its own, and the
*true* loss becomes visible again. There is no separate "ownership handoff"
logic needed; staleness alone makes this self-healing.

### BUG FIX: "stale, live via another target" firing with only one target running

The stale-hop display (`stats.cpp`'s `HopStats::compute()`) uses a second,
lighter-weight query, `shared_last_seen_from()`, to answer "is *anyone*
still hearing real replies from this exact responder IP right now" —
independent of the edge-keyed `map` above, since a hop can legitimately go
silent on ONE target's specific edge (asymmetric/ECMP per-flow routing)
while a *different* target's edge to the same public IP keeps getting real
replies. This backs the GUI's "(stale, live via another target)" badge.

That query used to be answered from a flat `ip_last_seen: unordered_map<ip,
timestamp>` index with no publisher attribution at all — "has anyone,
including me, published from this IP in the last 30s". A hop's own last real
reply is recorded as `stale_since` at the exact moment it goes stale, so it
is *always* inside that 30s window by construction — meaning this check
would routinely report "seen elsewhere" using nothing but the hop's own
immediately-prior reply, even with a single target running and no other
session ever having touched that IP. Reported live and confirmed exactly
this way.

Fixed by giving the index owner attribution too
(`IpLastSeen{ts, owner_session_id}`) and having `shared_last_seen_from()`
take the querying session's own id, excluding an entry whose owner is the
caller itself — the same owner-exclusion `shared_adopt_from()` already
applies to RTT adoption above, now applied consistently to the "seen
elsewhere" read as well. `stats.cpp` passes `HopStats::target_id_` (the same
id `Session::id_` publishes under) as the caller's own id. The legitimate
case — a genuinely different session's edge to the same IP still getting
real replies — is unaffected: verified with dedicated `tests/test_core.cpp`
cases (self-publish-then-self-query sees nothing; a different session's
publish is still surfaced; `max_age` expiry still applies regardless of
owner).

---

## 5. Why checksum pinning, and why the loop auditor never hard-stops (Pillar 3)

### The problem: a stuck/flapping link produces a "ghost train"

A dead or flapping WAN link can make a local device (home router, ISP CGNAT
box) answer TTL-limited discovery probes at **many** TTLs at once — the
packet keeps bouncing between a couple of real routers, incrementing TTL each
cycle, until it finally expires locally. Naively, the sliding discovery
window (`kDiscoveryWindow`, §7) can't tell this apart from genuine forward
progress: every "hop" answers, so the window keeps sliding all the way to
`max_hops`. Worse, once each of those duplicate-address hops is "answered,"
it **independently** starts its own full-rate direct-echo stream (§3) to the
*same* 1–2 real looping devices — and the shared-hop cache (§4) can't dedupe
this, because it's explicitly scoped to *cross-session* sharing
(`owner_session_id != self`); two hops within the *same* session pinging the
same IP is a same-session cascade, not the case the cache defends against.
Left unhandled this is up to `max_hops` independent full-rate probe streams
hammering the same real device — exactly the storm Pillars 1/4 exist to
prevent, just reintroduced from a different angle.

### Two rejected designs, and why

1. **Hard stop at the loop boundary.** Freezes that tail *forever* — a
   network recovery or a genuine route change past that point is never
   noticed again. Silent discovery must stay alive.
2. **Uniform backoff on every hop beyond the boundary, window left alone.**
   Still lets the discovery window keep sliding past the loop, because a
   loop's repeating replies still count as "resolved" and keep advancing the
   frontier — each of those hops still independently enters direct-echo
   steady state. This removes none of the cascade, it just adds a slow
   trickle underneath it.

### The actual fix: narrow the window, force the boundary to slow legacy probing

**Detection.** On every genuine legacy (TTL-elicited) reply, scan `hops_` for
any lower hop with the *same* resolved address:

```cpp
for (const auto& [h, hstat] : hops_) {
    if (h >= hop) break;                                  // hops_ iterates in increasing key order
    if (hstat.address() && *hstat.address() == inc.from) { dup_at = h; break; }
}
```

**Two-strikes confirmation** (`loop_confirm[{hop, dup_at}]`, needs `>= 2`
independent replies before freezing) — see the false-positive discussion
below for why a single sighting isn't enough.

**Window narrowing, not a stop.** `compute_max_hop()` (`session.hpp`, a pure
function so it's unit-testable without a live socket):

```cpp
uint8_t compute_max_hop(std::optional<uint8_t> dest_hop, std::optional<uint8_t> loop_at_hop,
                         uint8_t frontier, uint8_t max_hops) {
    if (dest_hop) return *dest_hop;
    int window_top = loop_at_hop ? (*loop_at_hop + kLoopAuditWindow)   // frozen: +1 hop only
                                  : (frontier + kDiscoveryWindow);      // normal: +3 hops
    return clamp(window_top, 1, max_hops);
}
```

Once confirmed, the fast window freezes at `loop_at_hop + 1` instead of
sliding to `max_hops` chasing the cycle — hops beyond the freeze are simply
never passed to `try_send()` at all, which alone kills the cascading
direct-echo storm (nothing is sent to them, so nothing can open a redundant
stream). Any of their rows briefly populated just before the freeze go stale
and prune via the engine's *existing* ghost-row logic — no new cleanup
needed.

**The one remaining in-scope hop (the audit hop)** is forced to (a)
**legacy-only** probing — `use_direct` gets an added
`&& !(loop_at_hop && ttl >= *loop_at_hop)` condition, specifically so it
doesn't open its own redundant direct-echo stream to whatever looping IP it
resolves to — and (b) the slow **audit cadence** `kLoopAuditSecs = 8.0`
instead of the normal interval. This is exactly what a person re-running
traceroute periodically to check whether a stuck hop has changed would do: a
real, low-rate, *never silent* probe.

**Self-healing, reply-driven, no timer.** The audit hop keeps getting a real
probe every 8s indefinitely. The moment its reply resolves to something that
is **not** a duplicate, the very same detection scan (which runs on *every*
reply, doubling as continuous re-validation) clears `loop_at_hop`, and the
next `max_hop` computation falls straight through to the normal sliding
window — discovery resumes at full speed automatically. If the loop has
simply moved one hop further out, the scan re-detects it there and re-freezes
— the audit point tracks forward on its own, with no separate re-detection
mechanism.

### Removing the false-positive source at the root, not just tolerating it

The realistic false-positive source here isn't random coincidence — it's
**ECMP path variance**, the well-documented artifact that "Paris traceroute"
(Augustin et al., 2006) was built to fix. Routers load-balancing across
parallel equal-cost links hash each packet's flow-identifying fields —
often *including the ICMP checksum* — to pick a branch. Since our probes to
different TTLs carry different `seq` values, and `seq` sits inside the
checksummed region, **different hops' probes to the same destination can
genuinely take different physical paths and land on the same real shared
router at two different hop counts — with no loop at all.**

**IPv4 fix — checksum pinning** (`build_echo`, `icmp.cpp`): reserve the last
2 bytes of the ICMP echo payload as an adjustment field. For a session, pick
a fixed target checksum once (`paris_checksum_target_`, derived from
`now_secs() ^ id_ ^ 0xC5A5`, stable per session, `0xFFFF` avoided for
cosmetic reasons only). For every probe:

1. Zero the adjustment field and the checksum field, compute `c0 =
   checksum(packet)`.
2. Solve algebraically for the adjustment that forces the *final* checksum
   to the fixed target: `adj = ones_complement_add(~target, c0)` — this is
   exact RFC 1071 one's-complement algebra (the same end-around-carry fold
   `checksum()` already implements), not an approximation.
3. Write `adj` into the adjustment field, recompute the real checksum
   (which will now equal `target` regardless of `seq`).

```cpp
static inline uint16_t ones_complement_add(uint16_t a, uint16_t b) {
    uint32_t sum = uint32_t(a) + uint32_t(b);
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return uint16_t(sum);
}
// build_echo(): adj = ones_complement_add(~(*pin_checksum), c0);
```

Net effect: every probe in a session presents the **same** checksum to any
router hashing on it, so ECMP always routes them identically — hop 3 and hop
7 genuinely cannot land on the same device via path-hash variance anymore,
because from the network's point of view they're the same "flow." Needs
`payload_size >= 2` (default 56); an unusually small custom payload just
skips pinning — detection-only for that case, never a crash, never a silently
wrong packet.

**IPv6:** raw ICMPv6 sockets leave checksum computation to the kernel (noted
in `icmp.hpp`), so this exact trick isn't available at this layer — but the
gap is much smaller than it sounds: **RFC 6438 mandates IPv6 ECMP hash the
Flow Label** instead of ICMP payload/checksum, specifically to avoid this
exact class of bug. IPv6 traffic is structurally far less exposed to
probe-seq-driven path variance in the first place.

**Backstop everywhere:** the two-strikes confirmation (`loop_confirm`, needs
2 independent replies) catches whatever variance remains on IPv6, or on any
router that doesn't follow the common ICMP-checksum-hash convention — a
transient coincidence won't repeat on a second, independently timed probe; a
real loop will, every time.

**Scoping the self-heal correctly.** A clean reply only clears the loop state
when it's *at or beyond* the current `loop_at_hop` — an unrelated earlier
hop's routine periodic recheck (§3) coming back clean proves nothing about the
actual loop boundary and must not clear it prematurely (the recheck cadence
this refers to is now runtime-configurable — see §6's note below — but the
scoping rule itself doesn't depend on what that cadence currently is). The
loop state is
also unconditionally cleared the moment the real destination is genuinely
reached (`dest_hop_` set) — once the destination is known, discovery no
longer consults `loop_at_hop` at all (`compute_max_hop`'s early return), so a
stale loop message must not linger in `error_`.

---

## 6. Route-flap handling (independent of, but interacting with, the above)

If a hop's resolved address changes between two replies (`hs.address() &&
*hs.address() != inc.from`), that's a genuine mid-session reroute — a
different device now answers at this TTL. All per-hop state that was
specific to the *old* device is reset so the new device gets its own fair
discovery-and-probation cycle instead of inheriting stale assumptions:

```cpp
if (hs.address() && *hs.address() != inc.from) {
    tries[hop] = 0; next_send[hop] = 0.0;
    echo_ok.erase(hop); echo_silent.erase(hop); echo_miss.erase(hop);
}
```

This is untouched by any of the pillars above — none of them change the
*trigger* condition, only what runs alongside it (the loop re-scan and the
edge-keyed cache re-publish, both driven off the same reply).

### The OTHER route-change case: a silent new device, not a replying one

The flap-detection above only fires when a legacy probe gets a reply from a
*different* address. There's a second, harder case: a route changes and the
new device at that position simply **drops** the legacy TTL-elicited probe
instead of replying with any address at all. Nothing in the flap path above
ever runs, so the *old* address — still a real, globally-routable IP — keeps
sitting in `hops_` and keeps answering direct-echo probes aimed at it,
producing a hop that blends two different physical routes (nicknamed the
"Frankenstein hop" in the code). Once a hop is echo-confirmed, its own
TTL-elicited Time-Exceeded is normally probed only rarely (`hop_recheck_secs()`
— 30s by default, see §3's note above and the GUI's Settings window)
precisely so this case is still checked for, just cheaply.

**Guarded wipe.** A hop's legacy-probe misses are counted (`legacy_miss`),
and once `legacy_miss_threshold_default()` (2 by default) of them happen at
least `hop_recheck_secs()` (30s by default — the two are always kept equal)
apart, the hop's address is wiped and rediscovered from scratch — gated on
the hop's own recent loss staying below
`kGuardedWipeMaxRecentLossPct` (a hop that's already looking unhealthy isn't
"falsely healthy," so it's left alone rather than flapped) and capped at
`kMaxGuardedWipesPerHop` per physical device (`wipe_count`, which
deliberately survives the wipe it's counting, to stop a chronically bad hop
from being wiped forever). The wall-clock spread is what tells this apart
from ordinary jitter — a single legacy probe's timing can't do that on its
own.

### Force Recheck: a decisive verification burst, not a single nudged probe

**BUG FIX.** `force_recheck()` (user-triggered, "re-verify this target's
route now") used to only zero `hop_recheck_at`, which fires **exactly one**
legacy probe — `hop_recheck_at` is reset to `now` the instant it's sent — so
a single click could contribute at most one miss toward the
threshold/window pair above. That's a passive nudge, not a decisive check:
it could never resolve a stuck Frankenstein hop on its own, only shave time
off however long the automatic background cadence would have taken anyway,
contradicting the feature's own promise.

The fix seeds a short **burst** of `kForceVerifyProbes` (3) back-to-back
legacy probes per already-addressed hop, sent at the target's normal probe
cadence (a couple of seconds total, not a full recheck-interval wait) via
three new per-hop counters: `force_verify_remaining` (probes not yet sent),
`force_verify_outstanding` (sent but not yet resolved), `force_verify_misses`
(resolved as a miss). The `due_recheck` gate in the send loop stays true for
the whole burst instead of reverting after one probe. If every probe in the
burst comes back silent, that's treated as real-time evidence at least as
strong as the wall-clock-separated organic trigger — several independent,
clustered misses rule out ordinary jitter just as well as two misses a full
recheck-interval apart do — and the same
guarded wipe fires immediately, through the same loss/wipe-count safety
gates (an explicit request bypasses the *wait*, never the safety check). Any
reply anywhere in the burst clears all three counters and confirms the hop
as-is. All three maps are also cleared on a full route-context reset
(alongside `legacy_miss`/`hop_recheck_at`) so no stale burst state can
attach to an unrelated future measurement for the same hop.

### Why "auto refresh" can look like it does nothing, right after Force Recheck visibly works

Reported live: Force Recheck resumed a stale target, but the passive
background mechanism (there is no separate feature literally named "auto
refresh" — this refers to the guarded-wipe path just above, which is the
thing that is supposed to do this on its own) appeared not to. This is not a
missing feature; both paths converge on the exact same guarded-wipe code and
the exact same safety gates. The difference is purely how fast each one can
reach a verdict:

- **Force Recheck**: 3 probes at the session's normal cadence (default 1s)
  → verdict in a few seconds.
- **Passive path**: 1 legacy probe per `hop_recheck_secs()` (30s by
  default), needs `legacy_miss_threshold_default()` (2 by default) of those
  to miss, spanning at least the same interval again → **at least ~60
  seconds** by default, often more depending on where in that cycle the
  route actually changed.

So watching a stale hop for anything under roughly a minute before
concluding "auto refresh didn't do it" will reliably look broken even when
it is working exactly as designed — it just hasn't reached its threshold
yet. This was originally 45s/45s (a ~90s minimum) when this section was
first written; both numbers are now 30s by default and, since a later
round, genuinely configurable from the GUI's Settings window — see
`set_default_recheck_tuning`'s doc comment (session.hpp) — so the exact
minimum wait is whatever's currently configured there (visible in the
Settings window itself), not a fixed number baked into this document.

---

## 7. Rate governance: two cooperating layers

### Per-target token bucket (`kMaxProbeRate = 30/s`, `kProbeBurst = 4`)

Bounds **one** target's own send rate to a smooth, low, constant stream —
capped at or below what the app already produces at steady state with a full
hop list. This is what keeps fast discovery from ever emitting a burst larger
than normal monitoring (no flood/scan signature) while still finding the
route quickly, by spending the limited budget preferentially on
unanswered hops (round-robin, so a run of non-responders can't starve hops
nearer the destination).

### Process-global pacer (`GlobalPacer`, `kPerTargetRate = 30/s` scaled by
active target count, hard ceiling `kPacerCeil = 500/s`)

The per-target bucket alone caps *one* target — with N targets, the
*aggregate* send rate onto any hop they all share (home router, BNG) is
`N × 30/s`, which blows past any router's control-plane ICMP rate limit long
before N gets large. `GlobalPacer::try_take()` requires a *second*,
process-wide token for every single send, so the aggregate can never exceed
the pacer's own rate regardless of how many targets are running. The rate
scales up with active target count (so a handful of targets aren't
needlessly throttled) but is clamped to `kPacerCeil` so it never permits
a flood-scale aggregate. Sessions join/leave the pacer's `active` count via a
small RAII guard (`PacerMembership`) so an early return from `run()` still
correctly decrements it.

A send must satisfy **both** the per-target bucket **and** a global pacer
token. The shared-hop cache (§4) is what actually reduces the number of
sends needed in the first place; the pacer is the hard backstop that bounds
whatever's left.

### Discovery cadence vs. steady-state cadence

Two related mechanisms, both already summarized in the README, worth
restating precisely here:

- **`kDiscoveryInterval = 0.35s`** — unanswered hops retry at this faster
  cadence (capped by the per-target/global buckets above) so the route is
  found quickly; answered hops fall back to the user's configured `interval`.
  Non-responders back off to the steady interval after `kDiscoveryTries` (5)
  fast attempts, so a `*` hop stops hogging the paced budget forever.
- **`kDiscoveryWindow = 3`** — while the destination is unknown, only probe
  up to 3 hops past the deepest already-resolved hop, sliding outward as hops
  resolve, rather than blasting every TTL up to `max_hops` simultaneously.
  This matters most for rate-limiting *destinations* (notably some IPv6
  endpoints): probing every TTL ≥ the real distance at once means they **all**
  reach the destination together, its own rate limiter drops most of that
  burst, and the resulting loss pollutes the destination's row for the whole
  focus window.

---

## 8. Reverse DNS: a dedicated pool, never on a probe thread

`getnameinfo()` (PTR lookup) blocks — often for seconds against a hop with no
PTR record or a slow/non-responding resolver. Running it on a probe thread
would stall that target's probe scheduling and inflate every RTT reading
behind it. `RdnsResolver` (`session.cpp`) is a single shared work
queue/cache serviced by **6** background worker threads:

```cpp
struct RdnsResolver {
    static constexpr int kWorkerCount = 6;
    std::mutex mtx; std::condition_variable cv;
    std::deque<std::string> queue;
    std::unordered_set<std::string> queued;              // in queue or in flight, dedup
    std::unordered_map<std::string, std::string> cache;   // ip -> hostname, "" = looked up, no PTR
    void worker() { for (;;) { /* wait, pop, blocking getnameinfo() OUTSIDE the lock, cache */ } }
};
```

One resolver for the whole process also deduplicates identical IPs across
every target — the router is resolved once, not once per target. 6 workers
(not 1) means a single slow/hanging PTR lookup can stall at most 6 concurrent
lookups, not the entire queue — `condition_variable::wait()` with a
predicate is safe with multiple independent waiters with no change to the
synchronization strategy, only how many `worker()` loops run it.

This is also what resolves **LAN** hop hostnames (e.g. a home router's own
`dnsmasq` PTR record, `RT-XXXX.home.arpa`) — unlike an earlier, never-called
`resolve_some_hostnames()` that only handled public hops, every discovered
address (private or public) is enqueued and resolved.

`Session::run()`'s own loop only ever *reads* the ready cache (`lookup()`) and
enqueues IPs it hasn't named yet, at most once a second — it never blocks on
a lookup itself.

---

## 9. Resilience: route change, outage, interface change

The user-facing requirement is that discovery survives all three without
manual intervention. Each is handled by an existing, narrow mechanism, none
of which the pillars above needed to touch except at the margins:

- **Route change (a different device answers a known hop):** the route-flap
  reset in §6, triggered purely by an address mismatch on a genuine reply.
- **Total outage (no replies at all):** the existing timeout/frontier-decay/
  ghost-pruning logic. `max_hop_seen_` only reflects hops that answered
  **recently** (`kFrontierStaleSecs = max(30, interval × 20)`), not ever — a
  single stray reply during a flap can't pin the frontier (and the ghost row
  it created) in place forever. Once a destination is confirmed, rows
  `1..dest_hop_` are kept contiguous and everything beyond is pruned; while
  still discovering, ghost rows (an address seen once, now silent, beyond the
  decayed frontier) are dropped. The loop auditor's audit-hop backoff (§5)
  means even a stuck tail keeps getting a real slow probe throughout an
  outage, so recovery is noticed as soon as the network returns — discovery
  is never permanently silent.
- **Interface/family/privilege change (live settings edit):** `rebuild_needed_`
  (set by `update_settings()` when family/source/privileged differs) drives a
  full discovery-state reset at the top of the next loop iteration —
  `pending`, `hops_`, `dest_hop_`, the echo-state maps, the loop-auditor
  state, everything clears and discovery restarts from scratch against a
  freshly `acquire_sock()`'d pooled socket for the new combination. Releasing
  the old `shared_ptr<Prober>` here lets the pool ref-count the old socket
  away if no other session still uses that exact combo — same lifecycle the
  pre-pool exclusive-ownership model had, just shared instead of exclusive.

### Family availability: don't probe blindly into a family with no real egress

Before starting to probe, `Session::run()` checks whether the local machine
actually has a usable egress for the target's family — `has_local_v4`/
`has_local_v6`, derived from `list_interfaces()`. For an explicit `Family:
IPv6` (or `V4`) choice with no usable egress, it shows a clear "No local
IPv6 egress available — waiting for IPv6 or change family" message and
polls for up to 30s for one to appear (an interface coming up, or the user
changing the setting) rather than opening a socket and emitting a burst of
probes that can only ever time out. `Auto` uses the same two booleans to
prefer whichever family has both a DNS answer and a real local egress,
falling back to IPv4 first as before if both exist.

**BUG FIX.** "Usable egress" used to mean "any interface has an address of
this family, at all" — which includes **link-local** (`169.254.0.0/16`
IPv4 APIPA, `fe80::/10` IPv6). Link-local addresses are auto-assigned to
every active interface by SLAAC/APIPA on every major OS **regardless of
real internet connectivity** — IPv6 SLAAC in particular does this
unconditionally — so this check passed on virtually every machine whether
or not it actually had a route anywhere. An explicit `Family: IPv6`
selection on an otherwise IPv4-only machine (link-local v6 present, no real
v6 route) would skip straight past the friendly wait message and open a
real socket against a destination it could never reach, producing confusing
0%-progress/100%-loss behavior instead of a clear explanation. Fixed by
filtering through `is_cacheable_ip()` (§4's private-IP cache gate reuses
the same function) before counting an interface address toward
`has_local_v4`/`has_local_v6` — it already excludes link-local, loopback,
and the unspecified address while still correctly accepting private/CGNAT
addresses (a home LAN behind NAT is a perfectly real, usable egress, same
reasoning as §4's private-IP cache fix).

---

## 10. Testability without a live socket

Several pieces of logic were deliberately given **external linkage** (rather
than left as file-local lambdas/anonymous-namespace functions) specifically
so `tests/test_core.cpp` can exercise them directly, without needing a raw
socket or Administrator/root privileges:

- `compute_max_hop()` — the loop-narrowing/discovery-window decision, as a
  pure function of `(dest_hop, loop_at_hop, frontier, max_hops)`.
- `is_public_ip()` — the public/private classification gate.
- `is_cacheable_ip()` — the looser "meaningful, routing-domain-scoped
  address" gate used both by §4's private-IP cache fix and by the family-
  availability check above.
- `SharedSample` / `SharedHopTable` / `shared_publish_to()` /
  `shared_adopt_from()` — table-taking variants that operate on a
  test-local `SharedHopTable` instance, never the process-global singleton,
  so tests can't pollute each other or a real running session.
- `build_echo()`'s checksum-pinning path — tested by building two packets
  with different `seq` and the same `pin_checksum`, asserting identical
  checksums (the actual property being fixed) plus the existing
  self-consistency check (`checksum(pkt) == 0` for any correctly-checksummed
  packet).

This is why the unit-test suite (`ctest` / `netpulse_tests`) can validate
every pillar's *decision logic* in a non-elevated CI/dev shell, even though
the actual sending/receiving of ICMP packets requires raw-socket privileges
that unit tests deliberately don't exercise.

---

## 11. The CLI (`cli/`, target `npulse`): a second front-end over the same engine

`cli/main.cpp` is a terminal front-end over the exact same engine calls the
Tauri desktop app uses — `Session` for `tracert`/`traceroute`/`tracert-mtr`/
`mtr`, `PingRun` for `ping`, `list_interfaces()` for `ifconfig`/`ipconfig` —
not a reimplementation of any probing logic. See `cli/CLI.md` for the full
command reference; this section covers the architectural decisions
specific to it.

**Separate executable, same build.** `cli/CMakeLists.txt` adds an `npulse`
target linked against `netpulse_core`, included via `add_subdirectory(cli)`
in the top-level `CMakeLists.txt`, gated by `NETPULSE_BUILD_CLI` (default
ON). This means `netpulse_tests` and `npulse` are both produced by the exact
same `cmake --build .` a contributor already runs for engine work — no
separate CLI-specific build step to remember, and no risk of the CLI
silently going stale against engine API changes the way a totally separate
repository/build would risk.

**`argv[0]`-based command dispatch.** Rather than a single `npulse
[OPTIONS] HOST` flag set, the binary inspects its own invoked name first
(stripping a path and a Windows `.exe` suffix) and dispatches to a
subcommand directly if it recognizes it (`ping`, `tracert`/`traceroute`,
`mtr`, `ifconfig`/`ipconfig`, `nslookup`) — falling back to `npulse
SUBCOMMAND ...` otherwise. This is what lets a copy or symlink of the
binary named `mtr` behave like `mtr` with no arguments needed beyond the
host, while the canonical `npulse` name still works identically via its
subcommand form — confirmed by literally copying the built binary under
each alias name and running it (see `CHANGES.md`'s verification section).

**`tracert` and `tracert-mtr` are two separate commands, not one with a
mode flag.** They're genuinely different output models over the same
`Session` engine: `tracert` (also `traceroute`) prints each hop exactly
once, in ascending order, once it "settles" (resolved, or 5 silent probes
with no reply — treated the same as real traceroute's own `* * *` row),
stopping at the destination or `--max-hops` — a one-shot process that
exits. `tracert-mtr` (also `mtr`) is the live, continuously-refreshing
table the GUI's Path/MTR view shows, and never exits on its own (barring
`-c`/`--report`). Splitting them avoids the far more common one-shot case
needing to remember a flag every time, and matches two real, differently-
named tools people already reach for instead of inventing a third
convention.

**Auto-refresh for `Family: Auto`, in place of Force Recheck.** The GUI's
Force Recheck is an interactive keypress against an already-running trace
— this CLI's live view has no keyboard-input reader loop to hang a key
binding on (see `CLI.md`'s "Not yet implemented" section). What
`tracert-mtr` has instead, specifically for `Family: Auto` (never for a
pinned `-4`/`-6`, which has no ambiguity worth silently overriding):
`Session::resolve()` — the family-availability check — runs exactly once,
at the very top of `Session::run()`, never re-evaluated mid-loop (see §9's
"Family availability" subsection). A trace started before a real route
came up would otherwise sit on a stale "no local egress" message forever.
`cmd_tracert_mtr()` instead wraps `Session` construction in an outer retry
loop: if a session under `Auto` reports an error continuously for more
than 15 seconds, the `Session` is destroyed and a fresh one constructed
(re-running `resolve()` from scratch), repeating every few seconds until
it succeeds or the user interrupts.

Combining "a real Ctrl-C" and "this internal restart decision" into the
one `std::atomic<bool>* stop` pointer `Session::run()` accepts needed a
small dedicated helper (`run_session_until()`, `cli/main.cpp`) — a
lightweight poller thread that sets a shared `combined` flag true the
moment either the global SIGINT flag or a local `want_restart` flag fires,
and that's what actually gets passed to `run()`. The caller can still tell
the two apart afterward by checking the global SIGINT flag specifically:
true means a real interrupt (stop for good), false means the internal
restart fired (loop again). Verified directly: `Auto` against a target
this project's own build sandbox genuinely can't reach retries every ~3s
as designed; the identical target with a family pinned explicitly shows
its error once and never retries; a live SIGINT sent mid-retry stops it
immediately — all three clean under AddressSanitizer/UBSan, including the
new poller thread.

**`ifconfig -w`/`--watch`** is the same idea applied to the adapter list
itself rather than a single in-progress trace — a plain redraw loop (no
threading needed, since there's no long-running `Session`/engine call to
combine a stop signal with) for noticing an adapter change over time (a
cable unplugged/replugged, a VPN connecting) that a one-shot CLI process
would otherwise have no way to observe.

**Why `-c`/`--count` on `tracert-mtr` is an approximation, not exact.**
`PingRun::run()` delivers one `PingLine` per sequence number — an exact,
well-defined unit to count. `Session::run()`'s `on_update` callback has no
equivalent: different hops resolve, get re-measured, and get evicted
(guarded wipe) on their own independent schedules, so there's no single
clean "one complete round" boundary the way a ping's sequence number is.
`cmd_tracert_mtr()` uses "a non-empty snapshot delivery" as its counting unit
instead — good enough for "stop once the trace has actually produced
something," not precise enough to promise "exactly N probe rounds per hop."
Documented as an approximation in `CLI.md` rather than oversold as exact.

**Packaging: a Tauri `externalBin` sidecar, not a separate download.**
`tauri.conf.json`'s `bundle.externalBin: ["binaries/npulse"]` bundles a
per-target-triple-named build of this binary into every installer Tauri
produces (NSIS/AppImage/deb/dmg), placed by a "Build CLI sidecar" step
added to each of `tauri-ci.yml`/`tauri-release.yml`/`tauri-canary-
build.yml`'s build jobs (see `tauri-app/src-tauri/binaries/README.md` for
the exact sequence, which was run and confirmed working on a real machine
before being written into any workflow file). Getting the bundled binary
onto PATH is handled per-OS, with genuinely different levels of confidence
per platform — see `CLI.md`'s "Packaging" section for the full breakdown
of Windows (NSIS/EnVar, written to match documented plugin behavior but
not verified against a real install), Linux .deb (`bundle.linux.deb.files`,
confirmed by Tauri's own documentation), Linux AppImage (no install step
exists to hook, by design of the format), and macOS (not yet solved —
`.dmg` has no script-execution point the way NSIS/dpkg do; the right fix is
an in-app "install to PATH" action, not an installer-time script).

**The `npulse` console, "VS Developer Command Prompt" style, on every OS.**
Asked for explicitly, across two rounds of clarification: not a custom
REPL, but literally the user's own shell — the same relationship VS
Developer Command Prompt has to `cmd.exe` (a shortcut that runs it with an
environment script applied first, not a bespoke shell language of its
own) — and, per the second clarification, triggered uniformly whether
`npulse`/`netpulse` is typed bare into an already-open terminal OR the
file is run directly (double-click, a shortcut, `./npulse`), on every OS,
not gated on any Windows-specific "was this console freshly allocated"
distinction the way an earlier iteration of this feature was. `main()`
checks, before any argument parsing, whether the canonical `npulse`/
`netpulse` invocation (not an alias — see below) received zero arguments;
if so, `launch_shell_console()` prepends this executable's own directory
to `PATH` (`own_executable_dir()` — `GetModuleFileNameA` on Windows,
`readlink("/proc/self/exe")` on Linux, `_NSGetExecutablePath` on macOS,
falling back to resolving `argv[0]` directly if none of those apply),
prints a short banner, and spawns the user's real shell inside the same
session — `%COMSPEC%` (`cmd.exe`) via `CreateProcessA`/`WaitForSingleObject`
on Windows, `$SHELL` (falling back to `/bin/sh`) via `fork()`/`execl()`/
`waitpid()` on POSIX — waiting for it and propagating its exit code as
`npulse`'s own. Running under an ALIAS name (`ping`, `tracert`, `mtr`,
`ifconfig`, ...) with no further arguments is unaffected by any of this —
that alias's own usage/behavior applies, matching the same reasoning
`cmd_ping()`/`cmd_tracert()` already had for a missing HOST argument;
`npulse help` or any other argument is likewise unaffected, since only
literally zero arguments to the canonical name enters this path at all.

Verified more rigorously than prior Windows-specific work in this project,
and — for the POSIX side — verified completely, not just cross-compiled:
a MinGW-w64 cross-compiler and Wine were installed specifically to move
past compile-only/manual review for this file's `_WIN32` branches, and the
Linux build was run directly in the environment this was built in.
Confirmed on Linux: a bare invocation spawns the real `$SHELL` (tested
explicitly with both `/bin/sh` and `/bin/bash`), the spawned shell's `PATH`
genuinely has `npulse`'s directory prepended (`which npulse` resolves it
from inside that shell), `npulse help` runs correctly from inside that
session, the spawned shell's exit code propagates back correctly, and
every alias with no arguments correctly shows its OWN behavior rather than
entering console mode. Confirmed on the cross-compiled `.exe` under Wine,
repeated multiple times for stability: the same bare-invocation console-
hosting works, `COMSPEC`/`PATH` resolve and propagate correctly, the
with-arguments path still bypasses console-hosting entirely — plus,
independent of this feature, real `ping` replies, real adapter enumeration
via actual `GetAdaptersAddresses` (not a stub), and real DNS resolution,
all against genuine network traffic. One isolated false alarm during this
process is worth recording: a single Wine run of the console-hosting path
appeared to hang, which turned out to be transient (five subsequent runs
were all clean) rather than a real bug — confirmed by writing and testing
an isolated minimal reproduction of the exact `CreateProcessA` pattern
first, which is what made it possible to tell a real bug apart from Wine
flakiness with confidence rather than guessing. This is real execution
evidence on both platforms, not a compile-only check — though the Windows
side is still not a substitute for testing on an actual Windows machine.

**BUG FIX, reported live: the console's own promise wasn't backed by
anything.** The banner told the person to "use `ping`/`tracert`/`mtr`/
`ifconfig` directly," but nothing anywhere ever created a FILE by any of
those names — `argv[0]`-based dispatch (main()'s alias handling) only ever
activates for a name that actually exists somewhere on `PATH`, and the
first version of this feature only ever prepended `npulse`'s OWN
directory, which contains a file named `npulse` and nothing else. Typing
`tracert-mtr host` inside the console correctly failed with "not
recognized" — the shell was right, the feature was incomplete.
`setup_alias_dir()` fixes this for real: a session-scoped temporary
directory containing a hard link to the `npulse` executable under each
standard-command name (`kAliasNames`: `ping`, `tracert`, `traceroute`,
`mtr`, `tracert-mtr`, `ifconfig`, `ipconfig`, `nslookup`), prepended to
`PATH` ahead of everything else, and removed by `cleanup_alias_dir()` the
moment the spawned shell exits. A hard link (`CreateHardLinkA` on Windows,
`link()` on POSIX, falling back to a plain file copy only if that fails —
most likely a different filesystem/volume than the executable) costs zero
extra disk space and, unlike a symlink, needs no elevated privilege on
Windows. Shadowing the system's own `ping`/`tracert`/etc. only for the
lifetime of one console the person explicitly opened for this purpose is a
fundamentally different, much safer case than the PERMANENT PATH install
this project already decided against elsewhere (§ above, and `CLI.md`'s
Packaging section) — the same reasoning `conda activate`/`nvm use`/a
Python virtualenv already rely on.

Verified precisely against the reported failure: on Linux, confirmed
`which tracert-mtr`/`ping`/`tracert`/`mtr`/`ifconfig` inside a spawned
session all resolve into the new alias directory, confirmed running
`tracert-mtr HOST` by name inside that session produces correct output,
and confirmed the alias directory is fully removed after the session ends
— all clean under ASan/UBSan. On the cross-compiled Windows `.exe` under
Wine: confirmed `CreateHardLinkA` itself works correctly in isolation, and
confirmed a renamed/hardlinked copy of the executable invoked DIRECTLY
under an alias name dispatches and runs correctly end to end. What could
NOT be confirmed under Wine: the full nested chain (`npulse.exe` → spawns
`cmd.exe` → which spawns the hardlinked alias binary) hung reliably,
despite every individual link in that chain checking out correctly on its
own — most consistent with a Wine-specific limitation in its console/
process emulation for a process spawning a shell that itself spawns
further raw-socket-using processes, but this is not certain, and could not
be verified against real Windows. Documented plainly as an open question
in `CLI.md` rather than presented as resolved.

**BUG FIX, reported live: a long hostname could hide its own destination
marker.** `render_hop_table()`'s `[DEST]` suffix used to be appended to
the hop's display string BEFORE truncating it to the Host column's width
— so a fully-resolved IPv6 reverse-DNS hostname (routinely long enough on
its own to need truncating) could cut the marker off partially or
entirely, on exactly the row where knowing "this is the destination"
matters most. Fixed by truncating the hostname/address text first, to a
budget that reserves room for the suffix, then appending the suffix after
— it can no longer be truncated away regardless of hostname length. The
Host column was also widened (38 → 46 characters, `kHostColumnWidth`) since
the same live report showed how routinely a real hostname+address
combination exceeds a narrow budget on its own, making truncation the
common case rather than a rare edge case.

**BUG FIX, reported live: `tracert-mtr`/`ifconfig -w` could corrupt the
parent shell's prompt, PowerShell specifically.** Running either directly
from an already-open PowerShell session (not through the `npulse` console
above, which hosts `cmd.exe` and was reported unaffected) could leave
fragments of unrelated text appearing to type themselves at the next
prompt, spurious continuation prompts, arrow keys inserting garbage. Root
cause is a well-documented class of Windows-console pitfall: a live-redraw
view repositioning the cursor inside the SAME screen buffer a line editor
(PSReadLine, or any shell's own line editing) is also tracking desyncs
that editor's bookkeeping from where the cursor actually is, and once
desynced, ordinary typed input can render or get interpreted incorrectly.
`cmd.exe` has no equivalent line-editing layer to desync, which is exactly
why it was unaffected.

Fixed with the standard technique for exactly this: `enter_live_view()`/
`leave_live_view()` (`cli/main.cpp`) switch to the **alternate screen
buffer** (`\033[?1049h`/`\033[?1049l`) once around the whole live session,
rather than doing per-frame cursor repositioning in the same buffer the
shell's own prompt lives in — the same guarantee `vim`/`less`/`htop`
already give on quit, on every exit path including Ctrl-C, not just a
clean one (enforced by always calling `leave_live_view()` before
`cmd_tracert_mtr()`/`cmd_ifconfig()`'s watch loop returns, regardless of
which branch got there). Two further defensive layers, added because this
class of bug is worth being thorough about rather than fixing only the
one reported symptom: `term_restore()` (registered once via `atexit()`,
so it runs on every exit path without a matching call needed at each
`return` in the file) saves the Windows console's `ENABLE_VIRTUAL_
TERMINAL_PROCESSING` mode before `term_init()` changes it and restores
the original afterward — a child process leaving shared console state
changed is a separate, real way to confuse a parent shell, since the
console mode is a property of the console object both processes share,
not a private copy — and discards any input the terminal sent but this
process never read (`FlushConsoleInputBuffer` on Windows, `tcflush` on
POSIX), since response bytes a terminal can send for certain queries can
otherwise resurface as garbage in whatever reads input next.

**A visible sign of being "inside" npulse, asked for explicitly**: two
layers, `set_terminal_title()` (window/tab title — `SetConsoleTitleA` on
Windows, the OSC 0 escape sequence on POSIX; found written but never
actually called anywhere, the same "defined but dead" pattern `term_init()`
itself had earlier in this project's history, now wired into both the
console-hosting path and every direct live-view invocation) and
`prepare_prompt_plan()`/`set_console_prompt_indicator()` (prefixes the
`npulse` console's own spawned shell prompt with `[npulse] ` — a plain
`PROMPT` environment variable suffices for `cmd.exe`, but bash/zsh
required actually testing the naive approach to find that it doesn't
work: an interactive shell always re-runs its own startup file on launch,
which unconditionally overwrites any inherited `PS1` before ever showing
a prompt, confirmed by testing a genuinely interactive shell — a real pty
on both stdin and stdout, since piped input alone makes bash detect
itself as non-interactive and skip this path entirely, which made an
earlier, more naive test look like it worked when it wasn't really being
exercised. The real fix layers the prefix in AFTER the user's own startup
file runs, using each shell's own supported mechanism: bash's `--rcfile
FILE -i`, zsh's `ZDOTDIR` pointed at a temp directory).

Verified with a real pseudo-terminal on Linux — captured the actual
escape bytes emitted (not just visual inspection) and confirmed the
alternate-screen sequence and title are correct and appear/disappear at
the right moments, confirmed the `[npulse]`-prefixed bash prompt renders
correctly live, confirmed a real `SIGTERM` mid-session completes promptly
(~3 seconds, matching the signal timeout, not a hang) with the alternate
screen properly exited first — and on the cross-compiled Windows `.exe`
under Wine across multiple repeated runs. Two testing-environment
artifacts surfaced and were resolved by reproducing in isolation rather
than assumed to be real bugs: a graceful-shutdown test hung specifically
when wrapped in the `script` pty-recording utility (traced to `script`'s
own known signal-forwarding/pty-monitoring quirks — the identical command
completed correctly and promptly with `script` removed from the test),
and one single Wine run of an already-passing command appeared to hang
once, which did not reproduce across three immediate repeat runs.

---

## 12. File map

| File | Responsibility |
|---|---|
| `core/include/netpulse/icmp.hpp` / `core/src/icmp.cpp` | ICMP/ICMPv6 packet codec, RFC 1071 checksum, Paris-style checksum pinning. |
| `core/include/netpulse/transport.hpp` / `core/src/transport.cpp` | `Prober` (one raw/datagram socket), the socket pool, `list_interfaces()`. |
| `cli/main.cpp` / `cli/CMakeLists.txt` | The `npulse` CLI (§11) — argument parsing and terminal rendering over `Session`/`PingRun`/`list_interfaces`. |
| `core/include/netpulse/session.hpp` / `core/src/session.cpp` | Everything in this document: `Session::run()`, the RX dispatcher, the global pacer, the shared-hop cache, the rDNS pool, the loop auditor. |
| `core/include/netpulse/ping_run.hpp` / `core/src/ping_run.cpp` | `PingRun` — the standalone single-host Ping tool's engine. A second `IcmpOwner` implementation (see §2's registry) alongside `Session`, sharing the same pooled sockets and RX dispatcher rather than opening its own — no separate thread, no OS `ping` subprocess. |
| `core/include/netpulse/stats.hpp` | `HopStats`/`HopStat` — rolling per-hop RTT/loss stats over the focus window. |
| `core/include/netpulse/platform.hpp` | `ensure_winsock_ready()` — lazy, thread-safe Winsock init (see the comment in `transport.cpp` on why this replaced a namespace-scope static). |
| `tests/test_core.cpp` | Unit tests for every externally-linked pure function above. |
| `tauri-app/src-tauri/native/netpulse_ffi.cpp` | `cxx` boundary: input validation/clamping, `Manager` (one `Session` thread per target), JSON snapshot marshalling. |
| `tauri-app/src-tauri/src/` | Rust host: Tauri commands, IPC surface, CSP, window hardening — statically links the engine via `cxx_build`. |
| `tauri-app/src/` | React renderer (Vite) — UI only, no engine logic. |
| `web/` | Standalone React renderer (Vite) for the `server/`-hosted HTTP deployment target — UI only, no engine logic. |
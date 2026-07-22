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
`PingRun` (`ping_run.hpp` — the standalone Ping tool's engine, §11's file
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

**Periodic re-confirmation (`kHopRecheckSecs = 45s`).** Once a hop is on
direct-echo, its TTL-elicited identity is no longer probed every round — so a
silent mid-session reroute (a different device now answers at this position)
would go unnoticed forever, since we'd just keep pinging the *old* device's
IP. Every 45s, one legacy TTL-limited probe is sent instead, re-confirming (or
updating) the hop's real address — deliberately rare (well under 3 probes/sec
even summed across 100 targets sharing a hop) and, unlike the interval
measurement, **never satisfied by shared-hop adoption** (§4) since its whole
point is independent re-verification.

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

### Gate 1 — public IPs only

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
principle.

`is_public_ip()` (`session.cpp`, external linkage, mirrors
`web/src/bgp.js`'s `isPublicIp()` exactly — same exclusion ranges: `10/8`,
`127/8`, `169.254/16`, `172.16–31/12`, `192.168/16`, CGNAT `100.64/10`, IPv6
`fe80::/10`, `fc00::/7`) gates **both** `shared_publish_to` and
`shared_adopt_from`. Private hops (including the user's own router) are
still measured — direct-echo still applies, no rate-limit risk there since it
never elicits a control-plane error — they're just never cross-target-cached,
so a private-IP collision can never cross-contaminate.

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

**Finding the predecessor robustly.** The naive approach —
`hops_[hop-1].address()` — breaks the moment hop-1 doesn't answer a
TTL-limited probe (many real routers silently forward without ever
generating Time-Exceeded, while still forwarding fine). If hop-1 is
unresolved, that does **not** mean the whole predecessor chain is unknown —
hop-2 (or further back) may well be resolved and is the correct, stable
predecessor. `predecessor_of()` therefore walks backward from `hop-1` down to
hop 1 and uses the **nearest resolved** hop's address, falling back to the
sentinel `"SRC"` only if *no* lower hop has ever resolved:

```cpp
auto predecessor_of = [&](uint8_t hop) -> std::string {
    for (uint8_t h = hop; h-- > 1;) {           // h = hop-1, hop-2, ..., 1
        auto it = hops_.find(h);
        if (it != hops_.end() && it->second.address()) return *it->second.address();
    }
    return kSharedHopSrcSentinel;                // "SRC" — nothing below this hop has ever resolved
};
```

Without this walk-back, a route whose hop-1 happens to be silent would get a
*different* (sentinel-based) key than the exact same route once hop-1 starts
answering — splitting one stable cache entry into two — and could
coincidentally collide with another target whose *own* hop-1 is silent for an
unrelated reason, since both would fall back to the same `"SRC"` sentinel at
the same responder.

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
hop's routine 45s recheck (§3) coming back clean proves nothing about the
actual loop boundary and must not clear it prematurely. The loop state is
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

---

## 10. Testability without a live socket

Several pieces of logic were deliberately given **external linkage** (rather
than left as file-local lambdas/anonymous-namespace functions) specifically
so `tests/test_core.cpp` can exercise them directly, without needing a raw
socket or Administrator/root privileges:

- `compute_max_hop()` — the loop-narrowing/discovery-window decision, as a
  pure function of `(dest_hop, loop_at_hop, frontier, max_hops)`.
- `is_public_ip()` — the public/private classification gate.
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

## 11. File map

| File | Responsibility |
|---|---|
| `core/include/netpulse/icmp.hpp` / `core/src/icmp.cpp` | ICMP/ICMPv6 packet codec, RFC 1071 checksum, Paris-style checksum pinning. |
| `core/include/netpulse/transport.hpp` / `core/src/transport.cpp` | `Prober` (one raw/datagram socket), the socket pool, `list_interfaces()`. |
| `core/include/netpulse/session.hpp` / `core/src/session.cpp` | Everything in this document: `Session::run()`, the RX dispatcher, the global pacer, the shared-hop cache, the rDNS pool, the loop auditor. |
| `core/include/netpulse/ping_run.hpp` / `core/src/ping_run.cpp` | `PingRun` — the standalone single-host Ping tool's engine. A second `IcmpOwner` implementation (see §2's registry) alongside `Session`, sharing the same pooled sockets and RX dispatcher rather than opening its own — no separate thread, no OS `ping` subprocess. |
| `core/include/netpulse/stats.hpp` | `HopStats`/`HopStat` — rolling per-hop RTT/loss stats over the focus window. |
| `core/include/netpulse/platform.hpp` | `ensure_winsock_ready()` — lazy, thread-safe Winsock init (see the comment in `transport.cpp` on why this replaced a namespace-scope static). |
| `tests/test_core.cpp` | Unit tests for every externally-linked pure function above. |
| `tauri-app/src-tauri/native/netpulse_ffi.cpp` | `cxx` boundary: input validation/clamping, `Manager` (one `Session` thread per target), JSON snapshot marshalling. |
| `tauri-app/src-tauri/src/` | Rust host: Tauri commands, IPC surface, CSP, window hardening — statically links the engine via `cxx_build`. |
| `tauri-app/src/` | React renderer (Vite) — UI only, no engine logic. |
| `web/` | Standalone React renderer (Vite) for the `server/`-hosted HTTP deployment target — UI only, no engine logic. |
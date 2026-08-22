#include "netpulse/session.hpp"
#include "netpulse/transport.hpp"
#include "netpulse/platform.hpp"
#include "netpulse/obfuscate.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <chrono>
#include <cstring>
#include <deque>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX  // keep <windows.h> from defining min()/max() macros
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#endif

namespace netpulse {

// ---------------------------------------------------------------------------
// Cross-session reply registry.
//
// Historically (before the socket pool below), each target ran its own raw
// ICMP socket, and Windows would often deliver an inbound reply to only ONE
// of those sockets rather than copying it to all — so a reply meant for
// target B could land on target A's socket and be silently discarded (wrong
// ICMP id), leaving B's shared hops (home router, ISP BNG, common
// intermediates) reading high/100% loss even though the router answered.
//
// The socket pool (see PooledSocket/acquire_pooled_socket below) removes the
// ROOT CAUSE: targets sharing a (family, source_addr, privileged) combo now
// share the exact same socket, read by exactly one RX dispatcher thread — so
// there's no more "which socket did the OS hand it to" ambiguity to begin
// with. This registry is what the dispatcher uses to find the right owner:
// it maps each ICMP id -> IcmpOwner* (session.hpp), and every incoming reply
// (regardless of which pooled socket it arrived on) is looked up here and
// forwarded to that owner's push_incoming() — the only way anything ever
// receives a reply now; nothing reads sockets directly except the
// dispatcher itself. Originally Session-only (map value used to be
// Session*); generalized to IcmpOwner* so PingRun (ping_run.hpp) can share
// this exact same registry/dispatcher instead of standing up a second,
// parallel receive mechanism.
namespace {
std::mutex g_reg_mtx;
std::map<uint16_t, IcmpOwner*> g_registry;
} // namespace

void register_icmp_owner(uint16_t icmp_id, IcmpOwner* owner) {
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    g_registry[icmp_id] = owner;
}

void unregister_icmp_owner(uint16_t icmp_id, IcmpOwner* owner) {
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    auto it = g_registry.find(icmp_id);
    if (it != g_registry.end() && it->second == owner) g_registry.erase(it);
}

bool icmp_owner_port_in_use(uint16_t base_port, int span) {
    // See this function's own doc comment (session.hpp). A candidate block
    // is "in use" if ANY port within [base_port, base_port+span) currently
    // has a live registry entry — g_registry is keyed by the exact
    // confirmed port each probe actually bound to, so this is a real
    // occupancy check, not a guess based on what was originally requested.
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    auto it = g_registry.lower_bound(base_port);
    return it != g_registry.end() && it->first < static_cast<uint32_t>(base_port) + span;
}

// Called ONLY by the shared RX dispatcher thread (see rx_dispatch_loop below),
// for every reply read off any pooled socket. Public (not private) solely
// because the dispatcher is a free function living outside the Session class
// — mirrors resolve() already being public "for tests" in spirit: internal
// plumbing exposed out of necessity, not meant for general callers. Kept as
// the dispatcher's call site (unchanged name/signature) even though the
// actual lookup is now owner-type-agnostic — see register_icmp_owner above.
void Session::dispatch_incoming(const Incoming& inc) {
    // DIAGNOSTIC (NETPULSE_DEBUG=1): log EVERY ICMP packet that reaches this
    // dispatcher, BEFORE the registry lookup below can drop it. This is the
    // single most decisive datum for the long-standing "UDP mode discovers
    // nothing on Windows even when running elevated" problem, because it
    // cleanly separates the only two possible causes, which otherwise look
    // identical from the UI (all hops 0 sent / 0 recv):
    //
    //   * NOTHING logged here while UDP probes are going out  => the OS is
    //     not delivering the routers' ICMP Time-Exceeded to this raw socket
    //     at all. That is a platform/socket-setup problem (on Windows a raw
    //     socket generally must be bound to a real local interface address
    //     to receive, and this one is left unbound whenever the interface is
    //     "Auto"; ICMP mode can still work in that state because its replies
    //     are echo replies to this host's own probes). No amount of
    //     correlation fixing helps in this case.
    //
    //   * Packets ARE logged here but get dropped just below (id not in the
    //     registry) => delivery is fine and this is purely a correlation
    //     bug: the id we registered doesn't match the embedded source port
    //     the router quoted back. Fixable entirely in this codebase.
    //
    // Deliberately logs the id alongside, so the dropped value can be
    // compared directly against the udp_port the session reported
    // registering at startup.
    const bool dbg = debug_logging_on(); // see platform.hpp — env var OR the log file enabled at runtime
    IcmpOwner* owner;
    {
        std::lock_guard<std::mutex> lk(g_reg_mtx);
        auto it = g_registry.find(inc.reply.id);
        if (it == g_registry.end() || !it->second) {
            if (dbg) {
                char buf[256];
                snprintf(buf, sizeof(buf), "[netpulse-rx] DROPPED (no owner registered for local_port/id=%u) kind=%d from=%s seq=%u orig_proto=%u\n",
                         inc.reply.id, (int)inc.reply.kind, inc.from.c_str(), inc.reply.seq, inc.reply.orig_proto);
                debug_log(buf);
            }
            return; // no current owner (removed target, or a stray/duplicate packet) — safe to drop
        }
        owner = it->second;
    }
    if (dbg) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[netpulse-rx] routed local_port/id=%u kind=%d from=%s seq=%u orig_proto=%u\n",
                 inc.reply.id, (int)inc.reply.kind, inc.from.c_str(), inc.reply.seq, inc.reply.orig_proto);
        debug_log(buf);
    }
    owner->push_incoming(inc);
}

// Ceiling on a single session's inbox_ (below) — see push_incoming's own
// doc comment for the full rationale. Generous relative to realistic
// legitimate burst size (a session's own consuming loop drains this at
// least every ~100-200ms under normal operation, and even a very large
// max_hops/many concurrent targets sharing the dispatcher shouldn't
// legitimately queue more than a few dozen replies for any ONE session
// between drains) while still bounding worst-case growth to something
// trivially small in memory (Incoming is a short string + a small POD
// struct + a double — even kMaxSessionInboxSize of these is negligible).
constexpr size_t kMaxSessionInboxSize = 2000;

void Session::push_incoming(const Incoming& inc) {
    std::lock_guard<std::mutex> ib(inbox_mtx_);
    inbox_.push_back(inc);
    // BUG FIX: inbox_ previously had no bound at all. It's filled by the
    // single shared, process-wide RX dispatcher thread (dispatch_incoming
    // above) for EVERY session in the process, and drained only by this
    // session's own run()/run_udp() thread. Those are two independent
    // rates with no backpressure between them — if this session's own
    // consuming thread ever falls behind for any reason (descheduled under
    // load with many concurrent targets, stuck retrying something, or any
    // bug that stalls it even temporarily), the dispatcher would keep
    // unboundedly appending here forever: nothing about push_incoming()
    // itself ever slows down or refuses. A single stalled session could
    // silently grow without limit for as long as the stall lasts, which is
    // exactly the shape of problem that eventually shows up as the whole
    // process running out of memory with no single obvious allocation site
    // to blame. Capping this means a stalled session degrades gracefully —
    // it loses the oldest, most-stale-by-the-time-they'd-be-processed
    // replies instead of growing without bound — rather than becoming a
    // process-wide liability. Dropping from the FRONT (oldest) rather than
    // refusing the new one: a reply that just arrived is far more likely to
    // still be within any outstanding probe's timeout window than one that
    // has already been queued, unprocessed, for a long time.
    while (inbox_.size() > kMaxSessionInboxSize) inbox_.pop_front();
    inbox_cv_.notify_one(); // wake this session's run() if it's waiting on an empty inbox
}

Session::~Session() {
    unregister_icmp_owner(icmp_id_, this);
}

// ---------------------------------------------------------------------------
// Process-GLOBAL send pacer (the real "global cap").
//
// Each target runs run() on its own thread with its own per-target token bucket
// (see `tokens` in run()). That bucket alone caps ONE target at kMaxProbeRate,
// but it is a stack local — it knows nothing about the other targets. With N
// targets that means the aggregate ICMP send rate onto any SHARED early hop
// (your home router, the ISP BNG, common intermediates) is N * kMaxProbeRate:
// 100 targets at 0.5s each put ~3000 ICMP-eliciting probes/second on hop 1, far
// past the control-plane ICMP rate limit of a home router, so it drops the
// excess time-exceeded replies and that hop paints fake loss — even though it
// is forwarding everything fine. (The final destination, which each target
// probes alone, stays at 0% — the classic "loss at an intermediate hop, none at
// the end" traceroute artifact.)
//
// This bucket is the coordination the per-target buckets can't provide: every
// send must ALSO take one global token, so the whole process can never exceed
// g_pacer's rate no matter how many targets run. The rate SCALES with the
// number of active targets (so a handful of targets aren't throttled) but is
// clamped to a ceiling that stays well under what any single shared hop will
// rate-limit — kPacerCeil. Sessions join on entering the probe loop and leave
// on exit via a small RAII guard (so an early return still decrements the
// count). Self-contained critical sections, no nested locks -> no deadlock and
// no lock-ordering constraint against g_reg_mtx / inbox_mtx_.
namespace {
struct GlobalPacer {
    std::mutex mtx;
    int active = 0;      // number of sessions currently in their probe loop
    double tokens = 0;   // available global send tokens
    double last_refill = 0;
    // Per-target fair-share rate, and the hard aggregate ceiling. The aggregate
    // is clamp(active * kPerTargetRate, kPerTargetRate, kPacerCeil): one target
    // gets its full rate, N targets share a smoothly larger (but bounded) budget
    // so shared hops never see a flood.
    static constexpr double kPerTargetRate = 30.0; // probes/sec credited per active target
    static constexpr double kPacerCeil     = 500.0; // probes/sec — hard aggregate ceiling
    static constexpr double kBurst         = 8.0;   // max accumulated tokens (small anti-spike allowance)

    double rate_locked() const {
        double r = kPerTargetRate * (active > 0 ? active : 1);
        return r < kPerTargetRate ? kPerTargetRate : (r > kPacerCeil ? kPacerCeil : r);
    }
    void join() { std::lock_guard<std::mutex> lk(mtx); ++active; }
    void leave() { std::lock_guard<std::mutex> lk(mtx); if (active > 0) --active; }
    // Try to spend one global token. Returns true if granted.
    bool try_take(double now) {
        std::lock_guard<std::mutex> lk(mtx);
        if (last_refill == 0) last_refill = now;
        double r = rate_locked();
        tokens += (now - last_refill) * r;
        if (tokens > kBurst) tokens = kBurst;
        last_refill = now;
        if (tokens >= 1.0) { tokens -= 1.0; return true; }
        return false;
    }
    void refund() { std::lock_guard<std::mutex> lk(mtx); tokens += 1.0; if (tokens > kBurst) tokens = kBurst; }
};
// Leaked (never-destroyed) singleton. These globals are touched by target
// threads that Manager joins at shutdown, and the resolver below owns a
// detached thread; a heap object that is never destructed removes any
// static-destruction-order race between those threads and this state.
inline GlobalPacer& g_pacer() { static GlobalPacer* p = new GlobalPacer(); return *p; }

// RAII: keep the pacer's active count accurate across every exit path of run().
struct PacerMembership {
    PacerMembership() { g_pacer().join(); }
    ~PacerMembership() { g_pacer().leave(); }
    PacerMembership(const PacerMembership&) = delete;
    PacerMembership& operator=(const PacerMembership&) = delete;
};
} // namespace

// ---------------------------------------------------------------------------
// Public/private address classification — mirrors web/src/bgp.js's
// isPublicIp() exactly (same exclusion ranges), so the engine and the UI agree
// on what counts as "public." Used below to gate shared-hop caching: see the
// rationale in the SharedHopTable comment for why only public IPs are safe to
// cross-target-cache. External linkage (declared in session.hpp), not inside
// the anonymous namespace below, specifically so tests/test_core.cpp can call
// it and the table-taking shared_publish_to/shared_adopt_from directly.
bool is_public_ip(const std::string& ip) {
    if (ip.empty() || ip == "*") return false;
    if (ip.find(':') != std::string::npos) {
        std::string lo = ip;
        for (char& c : lo) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lo == "::1" || lo == "::") return false;
        if (lo.rfind("fe8", 0) == 0 || lo.rfind("fe9", 0) == 0 ||
            lo.rfind("fea", 0) == 0 || lo.rfind("feb", 0) == 0) return false; // fe80::/10
        if (lo.rfind("fc", 0) == 0 || lo.rfind("fd", 0) == 0) return false;   // fc00::/7 ULA
        return true;
    }
    unsigned a = 256, b = 256, c = 256, d = 256;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    if (a == 10 || a == 127 || a == 0) return false;
    if (a == 169 && b == 254) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;
    if (a == 192 && b == 168) return false;
    if (a == 100 && b >= 64 && b <= 127) return false; // CGNAT 100.64/10
    if (a >= 224) return false; // multicast / reserved
    return true;
}

// ---------------------------------------------------------------------------
// Shared-hop publish/subscribe (the actual fix for router/intermediate loss).
//
// The pacer above bounds the TOTAL send rate, but the honest fix for a hop that
// many targets share is to stop probing it many times over. All targets on the
// same egress traverse the SAME first hops; there is no reason for 100 targets
// to each ping the router once per interval. Instead, whichever session is
// already probing a given responder IP publishes its latest real reply here,
// and the other sessions ADOPT that measurement as their own sample for that
// hop and skip their own send. The router then sees ~one probe per interval
// total instead of one per target -> genuine 0% loss on the shared hop even at
// 0.5s * 100 targets.
//
// PUBLIC IPs ONLY. A private/CGNAT address (192.168.x, 10.x, 100.64/10, …) is
// only unambiguous WITHIN one routing domain — the same private IP can be two
// entirely different physical devices behind different VRFs/NAT boundaries, so
// blindly sharing a measurement for one across targets can silently attribute
// one device's RTT to a completely different device. A public IP has no such
// ambiguity (globally unique by the internet's own addressing invariant), so
// cross-target sharing is always safe for it. Private hops (including the
// user's own home router) are still measured — direct-echo still applies, no
// rate-limit risk there since it never elicits a control-plane Time-Exceeded —
// just never cross-target-cached. See is_public_ip() above.
//
// Keyed by (source_addr, PREDECESSOR hop address, responder IP) — not just
// (source_addr, responder IP). Keying on the node alone conflates two
// different real situations: "the same physical router, reached the same way"
// (safe to share) vs. "the same public IP is visible from two genuinely
// different upstream path segments" (e.g. two regional edges converging on a
// shared core node, or genuine ECMP path variance — see the loop-auditor
// comment below for why probe-seq-driven ECMP variance is itself mitigated).
// Keying on the edge (predecessor -> responder) means targets that share the
// same path prefix still collapse onto one cache entry (the common, valuable
// case), while targets reaching the same node via a different predecessor get
// their own entry instead of silently overwriting each other's real numbers.
// If there's no lower hop yet (this IS hop 1) or the predecessor hasn't
// resolved, the predecessor component is the literal sentinel "SRC" — this
// hop is, from the network's perspective, directly reachable from our source.
//
// Every entry is attributed (owner session id, hop, predecessor) so a future
// debug surface can show exactly which target/path a cached sample came from,
// not just its value. Only REAL replies are ever published (never fabricated
// loss), and adoption only happens in steady state (hop address known, not
// during route discovery) using a sample fresher than ~1.5 intervals.
// Ownership is emergent and self-healing: if the owner's hop starts dropping
// it stops publishing, the entry goes stale within ~1.5 intervals, adopters
// fall back to real probing and see the true loss.
inline const char* shared_hop_src_sentinel() { return NETPULSE_OBF_STR("SRC"); } // predecessor placeholder for hop 1 / unresolved predecessor

// SharedSample/SharedHopTable and the table-taking shared_publish_to /
// shared_adopt_from have EXTERNAL linkage (declared in session.hpp), not
// inside the anonymous namespace below, specifically so tests/test_core.cpp
// can construct its own local SharedHopTable and exercise these directly —
// never the process-global singleton, so tests can't pollute each other or a
// real running session. g_shared()/shared_key()/the no-table-argument
// shared_publish()/shared_adopt() wrappers stay internal-linkage below since
// only the real probe loop in this file needs the process-global singleton.
inline std::string shared_key(const std::string& src, const std::string& predecessor, const std::string& ip) {
    std::string k;
    k.reserve(src.size() + predecessor.size() + ip.size() + 2);
    k += src; k += '\x1f'; k += predecessor; k += '\x1f'; k += ip;
    return k;
}
void shared_publish_to(SharedHopTable& sh, const std::string& src, const std::string& predecessor,
                       const std::string& ip, double ts, double rtt, uint64_t owner_session_id, uint8_t hop) {
    if (ip.empty() || !is_public_ip(ip)) return; // private/CGNAT hops are never cross-target-cached (see rationale above)
    std::unique_lock<std::shared_mutex> lk(sh.mtx);
    sh.map[shared_key(src, predecessor, ip)] = SharedSample{ts, rtt, owner_session_id, hop, predecessor};
    // Same event, second index — see ip_last_seen's doc comment in
    // session.hpp. Deliberately NOT gated on "did this improve on an
    // existing entry" — the latest publish is always the freshest, and a
    // plain last-writer-wins is exactly right for "is this IP alive right
    // now" regardless of which edge answered.
    sh.ip_last_seen[ip] = ts;
}
std::optional<double> shared_adopt_from(SharedHopTable& sh, const std::string& src, const std::string& predecessor,
                                        const std::string& ip, uint64_t self_session_id, double now, double max_age) {
    if (ip.empty() || !is_public_ip(ip)) return std::nullopt; // private/CGNAT hops always probe for real, never adopt
    std::shared_lock<std::shared_mutex> lk(sh.mtx);
    auto it = sh.map.find(shared_key(src, predecessor, ip));
    if (it == sh.map.end()) return std::nullopt;
    const SharedSample& s = it->second;
    if (s.owner_session_id == self_session_id) return std::nullopt; // we're the owner — probe for real
    if (now - s.ts > max_age) return std::nullopt;                  // stale — fall back to real probing
    return s.rtt;
}
std::optional<double> shared_last_seen_from(SharedHopTable& sh, const std::string& ip, double now, double max_age) {
    if (ip.empty() || !is_public_ip(ip)) return std::nullopt;
    std::shared_lock<std::shared_mutex> lk(sh.mtx);
    auto it = sh.ip_last_seen.find(ip);
    if (it == sh.ip_last_seen.end()) return std::nullopt;
    if (now - it->second > max_age) return std::nullopt;
    return it->second;
}

namespace {
inline SharedHopTable& g_shared() { static SharedHopTable* t = new SharedHopTable(); return *t; } // leaked singleton (see g_pacer note)

void shared_publish(const std::string& src, const std::string& predecessor, const std::string& ip,
                    double ts, double rtt, uint64_t owner_session_id, uint8_t hop) {
    shared_publish_to(g_shared(), src, predecessor, ip, ts, rtt, owner_session_id, hop);
}
// If another session has a fresh reply for this exact (source, predecessor, responder) edge, return its rtt (ms).
std::optional<double> shared_adopt(const std::string& src, const std::string& predecessor, const std::string& ip,
                                   uint64_t self_session_id, double now, double max_age) {
    return shared_adopt_from(g_shared(), src, predecessor, ip, self_session_id, now, max_age);
}
} // namespace

// External linkage (unlike shared_publish/shared_adopt above) — this is the
// one piece of the shared-hop cache callers OUTSIDE this file need (see the
// doc comment on the declaration in session.hpp): stats.cpp's stale-hop
// display has no SharedHopTable of its own and isn't part of the probe loop,
// it just wants to read the same process-global table the real sessions are
// already publishing into.
std::optional<double> shared_last_seen(const std::string& ip, double now, double max_age) {
    return shared_last_seen_from(g_shared(), ip, now, max_age);
}

// ---------------------------------------------------------------------------
// Process-GLOBAL reverse-DNS resolver.
//
// getnameinfo() blocks — often for seconds on a hop with no PTR record or a slow
// resolver — so it must never run on a probe thread (that would stall probe
// scheduling and inflate RTTs). Instead a POOL of background threads services a
// shared work queue and fills a shared cache; every session enqueues the IPs it
// sees and later reads the cache to set hop.hostname. One resolver for the whole
// process also dedups identical IPs across all targets (the router is resolved
// once, not once per target). Unlike the old (never-called) per-session
// resolve_some_hostnames(), this resolves ALL hops including private/LAN ones,
// so a home router with a PTR in its own dnsmasq (e.g. RT-BE86U-4528.home.arpa)
// gets named — exactly what Windows `tracert` shows and NetPulse previously did
// not.
//
// Pool size kWorkerCount, not a single thread: with 100+ targets, a single
// slow/hanging PTR lookup (a node that drops DNS queries and lets the request
// sit until the resolver's own timeout, often 5s+) would otherwise serialize
// EVERY other pending hostname behind it — 6 workers mean at most 6 outstanding
// slow lookups stall at once, and the rest of the queue keeps draining. std::
// condition_variable::wait() with a predicate is safe with multiple waiters —
// each is independently woken and re-checks the queue — so no change to the
// synchronization strategy is needed, only how many worker() loops run it.
namespace {
struct RdnsResolver {
    static constexpr int kWorkerCount = 6;
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::string> queue;
    std::unordered_set<std::string> queued;             // in queue or in flight (dedup)
    std::unordered_map<std::string, std::string> cache; // ip -> hostname ("" = looked up, no PTR)
    bool started = false;

    void ensure_started() {
        std::lock_guard<std::mutex> lk(mtx);
        if (started) return;
        started = true;
        for (int i = 0; i < kWorkerCount; ++i) {
            std::thread(&RdnsResolver::worker, this).detach(); // process-lifetime pool; no join needed
        }
    }
    // Enqueue an IP for resolution if we haven't seen it yet.
    void enqueue(const std::string& ip) {
        if (ip.empty() || ip == "*") return;
        std::lock_guard<std::mutex> lk(mtx);
        if (cache.count(ip) || queued.count(ip)) return;
        queued.insert(ip);
        queue.push_back(ip);
        cv.notify_one();
    }
    // Look up a resolved hostname (non-empty) if available.
    std::optional<std::string> lookup(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = cache.find(ip);
        if (it != cache.end() && !it->second.empty()) return it->second;
        return std::nullopt;
    }
    void worker() {
        ensure_winsock_ready();
        for (;;) {
            std::string ip;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [this] { return !queue.empty(); });
                ip = std::move(queue.front());
                queue.pop_front();
            }
            std::string host = reverse_dns(ip); // blocking; off the probe threads
            {
                std::lock_guard<std::mutex> lk(mtx);
                cache[ip] = host; // "" means "resolved, no PTR" so we don't retry forever
                queued.erase(ip);
            }
        }
    }
    // Blocking PTR lookup for one textual IP (v4 or v6).
    static std::string reverse_dns(const std::string& ip) {
        sockaddr_storage ss{};
        socklen_t slen = 0;
        if (ip.find(':') != std::string::npos) {
            auto* s6 = reinterpret_cast<sockaddr_in6*>(&ss);
            s6->sin6_family = AF_INET6;
            if (inet_pton(AF_INET6, ip.c_str(), &s6->sin6_addr) != 1) return "";
            slen = sizeof(sockaddr_in6);
        } else {
            auto* s4 = reinterpret_cast<sockaddr_in*>(&ss);
            s4->sin_family = AF_INET;
            if (inet_pton(AF_INET, ip.c_str(), &s4->sin_addr) != 1) return "";
            slen = sizeof(sockaddr_in);
        }
        char host[NI_MAXHOST] = {0};
        if (getnameinfo(reinterpret_cast<sockaddr*>(&ss), slen, host, sizeof(host),
                        nullptr, 0, NI_NAMEREQD) == 0) {
            // NI_NAMEREQD => only a real PTR name, never the numeric IP back.
            return std::string(host);
        }
        return ""; // no PTR record
    }
};
// Leaked singleton — owns a detached worker thread, so it MUST outlive every
// target thread and never be destructed (see g_pacer note above).
inline RdnsResolver& g_rdns() { static RdnsResolver* r = new RdnsResolver(); return *r; }
} // namespace

// ---------------------------------------------------------------------------
// Shared RX dispatcher (Pillar 1: the counterpart to the socket pool in
// transport.cpp). Exactly one background thread for the whole process reads
// EVERY pooled socket — no session ever calls recvfrom() itself anymore.
//
// Each iteration: snapshot the currently-alive pooled sockets
// (list_active_pooled_sockets(), transport.cpp), select() across all of their
// fds at once with a short timeout, and for every fd that's readable, drain
// it (Prober::drain_ready(), non-blocking — select() already proved it's
// readable) and hand each parsed reply to Session::dispatch_incoming(), which
// looks the ICMP id up in the SAME registry the old per-session
// route_reply_() used and pushes it straight into the owning session's inbox.
//
// The short (100ms) select timeout — rather than an unbounded wait — is what
// lets this loop notice a BRAND NEW pooled socket (e.g. the first target to
// use a not-yet-seen interface/family/privilege combo) promptly: the fd set
// is rebuilt from a fresh snapshot every iteration, so a new socket is picked
// up on the next iteration at the latest, ~100ms of extra latency on just its
// very first reply — inconsequential, and self-corrects immediately after.
namespace {
struct RxDispatcher {
    std::atomic<bool> started{false};

    void ensure_started() {
        bool expected = false;
        if (started.compare_exchange_strong(expected, true)) {
            std::thread(&RxDispatcher::run, this).detach(); // process-lifetime; no join needed
        }
    }

    void run() {
        ensure_winsock_ready();
        for (;;) {
            std::vector<std::shared_ptr<Prober>> socks = list_active_pooled_sockets();
            if (socks.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            fd_set rfds;
            FD_ZERO(&rfds);
            long long maxfd = -1;
            for (auto& s : socks) {
                long long fd = s->fd();
                if (fd < 0) continue;
                FD_SET(static_cast<unsigned>(fd), &rfds);
                if (fd > maxfd) maxfd = fd;
            }
            if (maxfd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000; // 100ms — see class comment for why this bounds new-socket pickup latency
            int sel = select(static_cast<int>(maxfd) + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue; // timeout (0) or a transient error (<0) — just re-snapshot and try again
            for (auto& s : socks) {
                long long fd = s->fd();
                if (fd < 0 || !FD_ISSET(static_cast<unsigned>(fd), &rfds)) continue;
                for (const auto& inc : s->drain_ready()) Session::dispatch_incoming(inc);
            }
        }
    }
};
inline RxDispatcher& g_rx() { static RxDispatcher* d = new RxDispatcher(); return *d; } // leaked singleton (see g_pacer note above)
} // namespace

// See this function's own doc comment (session.hpp) for why it needs to
// exist at all, outside the anonymous namespace g_rx() itself lives in.
void ensure_rx_dispatcher_started() { g_rx().ensure_started(); }

// Delay between successive hops' FIRST probe, so a fresh session ramps up
// gradually instead of bursting every hop's packet in the same instant. This
// is the same idea as PingPlotter's "Packet Send Delay" (Options > Engine).
// At 10ms across 30 hops the whole first round still ramps in 300ms —
// imperceptible, but enough to stop looking like a burst to a rate limiter.
constexpr double kSendStaggerSecs = 0.01;

// --- Route DISCOVERY vs steady-state MONITORING cadence -------------------
// Before the destination is reached, the visible route can only advance as
// fast as replies come back, and each hop only re-probes once per `interval`.
// At the steady 1s cadence, a single probe lost on the way to the destination
// freezes discovery for a whole second (several, if more are lost): the trace
// looks "stalled" (stuck at N hops / 0 hops with the spinner spinning) and
// then the full path snaps in "late" once a retry gets through. A related lag:
// even after the path is found, the destination's first kDiscoveryDropCount
// replies are consumed as settling, so at 1s spacing the RTT columns stay
// blank and the spinner stays up for ~kDiscoveryDropCount seconds.
//
// The fix is to re-probe UNANSWERED hops more often while discovering — but
// this must NOT turn into a high-rate packet burst. Raw-socket ICMP sent in
// fast bursts to a single address is exactly the pattern behavioral AV,
// Windows Defender's network monitor, and Smart App Control's heuristics flag
// as flooding/scanning. An earlier version of this that simply dropped every
// hop's retry interval to ¼s pushed the aggregate to ~120 packets/second and
// was (correctly) treated as hostile. So discovery here is governed by TWO
// mechanisms working together:
//
//   1. A per-hop DESIRED cadence — unanswered hops become "due" quickly
//      (kDiscoveryInterval), answered hops stay at the steady `interval`.
//   2. A per-TARGET token-bucket rate limiter (kMaxProbeRate / kProbeBurst)
//      that paces THIS target's sends into a smooth, low, constant stream and
//      prioritises the probes that actually advance discovery. This caps one
//      target's send rate at or below what the app already produced at steady
//      state with a full hop list (~max_hops/interval), so it never emits a
//      burst larger than normal monitoring — no flood/scan signature, while
//      still finding the route quickly because the limited budget is spent on
//      the hops that haven't answered yet (which, for any TTL >= the
//      destination's distance, reach the destination itself).
//
// Note the per-target bucket bounds ONE target. The process-wide g_pacer above
// bounds the AGGREGATE across all targets, and shared_adopt() suppresses
// duplicate sends to hops many targets share — both defined near the top of
// this file. A send must satisfy the per-target bucket AND take a g_pacer
// token; a hop already measured by another target this interval is adopted
// without sending at all.
//
// kDestSettleSecs keeps hops on the fast desired-cadence briefly AFTER the
// destination first answers, so its settling window clears in a fraction of a
// second instead of kDiscoveryDropCount·interval. The first-probe stagger
// (kSendStaggerSecs) still applies, so the very first round ramps rather than
// firing simultaneously.
constexpr double kDiscoveryInterval = 0.35; // s — desired retry cadence for UNANSWERED hops while discovering (gentle enough to avoid tripping carrier/ISP control-plane rate limiters on the Time-Exceeded burst)
constexpr int    kDiscoveryTries    = 5;    // fast attempts an unanswered hop gets before it's treated as a non-responder and backed off to the steady interval (stops `*` hops hogging the paced budget)
constexpr double kDestSettleSecs    = 1.0;  // s — keep fast cadence this long after the destination first answers
constexpr double kMaxProbeRate      = 30.0; // probes/sec — GLOBAL hard cap on ICMP sends (<= the app's own all-hops steady rate; keeps traffic ping-like, never a flood)
constexpr double kProbeBurst        = 4.0;  // max tokens — small burst allowance so sends stay a smooth stream, not spikes
// Bounded route-discovery window. While the destination hasn't been located we
// probe only up to kDiscoveryWindow hops PAST the deepest hop that has already
// resolved, and slide that window outward as hops resolve — rather than probing
// all max_hops TTLs at once. This matters most for rate-limiting endpoints
// (notably ICMPv6, e.g. Cloudflare): if every TTL >= the destination's distance
// is probed simultaneously, they ALL reach the destination, its rate-limiter
// drops most of that burst, and the loss pollutes the destination row for the
// whole focus window — which is exactly why an IPv6 hostname target flashed
// 100% loss at startup while IPv4 (whose destination doesn't rate-limit) did
// not. Walking outward means only ~one probe reaches the destination around the
// moment it's found, so it isn't hammered. (kDiscoveryWindow itself now lives
// in session.hpp, alongside kLoopAuditWindow and compute_max_hop() — see there.)
constexpr int    kFrontierProbes    = 2;    // sends after which an as-yet-unanswered hop still lets the window advance past it

// --- Routing-loop auditor ---------------------------------------------------
// A dead/flapping WAN link can make a local device (home router, ISP CGNAT)
// answer TTL-limited discovery probes at MANY TTLs at once — the packet keeps
// bouncing between a couple of real routers, incrementing TTL each cycle,
// until it finally expires locally. Left unhandled, the discovery window
// (kDiscoveryWindow above) can't tell this apart from genuine forward
// progress — every "hop" answers, so it keeps sliding all the way to
// max_hops, and once each of those duplicate-IP hops is "answered" it
// independently starts its own full-rate direct-echo stream to the SAME 1-2
// looping devices (the shared-hop cache can't dedupe this: two hops within
// the SAME session pinging the same IP aren't a cross-target case, they're a
// same-session cascade). This is the "ghost train" a bad link produces.
//
// Detection: whenever a hop resolves via a genuine legacy (TTL-elicited)
// reply, scan the hops already known at LOWER TTLs for the same address. A
// match means the path has folded back on itself.
//
// The realistic false-positive source for this isn't random coincidence —
// it's ECMP path variance, the well-documented artifact "Paris traceroute"
// (Augustin et al., 2006) was built to fix: routers load-balancing across
// parallel equal-cost links hash each packet's flow fields (often including
// the ICMP checksum) to pick a branch, so probes to different TTLs — which
// carry different `seq`, hence different checksums — can genuinely take
// different physical paths and land on the SAME real shared router at two
// different hop counts, with no loop at all. build_echo()'s checksum pinning
// (see icmp.hpp) removes this at the root for IPv4 by making every probe in a
// session present the same checksum, so ECMP always picks the same path.
// IPv6 doesn't need it: RFC 6438 mandates IPv6 ECMP hash the Flow Label
// instead. As a backstop for whatever variance remains (or a router that
// ignores the convention), a duplicate must be seen on TWO independent
// replies (loop_confirm below) before it's treated as a real loop — a
// transient coincidence won't repeat, a real loop will, every time.
//
// Response, once confirmed: NEVER a hard stop (silent discovery must stay
// alive so a route change or recovery is still noticed — see kLoopAuditSecs).
// Instead, compute_max_hop() (session.hpp) freezes the FAST discovery window
// right at the loop boundary (+kLoopAuditWindow, currently just 1 hop) instead
// of letting it keep sliding to max_hops chasing the cycle — this alone kills
// the cascading direct-echo storm, since hops beyond the freeze are simply
// never in scope for try_send() at all. The loop boundary hop itself (and the
// one audit hop past it) are forced to slow (kLoopAuditSecs), legacy-only
// probing — never direct-echo, so they never open their own redundant stream
// to the looping IP. The moment either position's reply comes back WITHOUT a
// duplicate, the loop clears itself — no timer, driven entirely by the reply.
constexpr double kLoopAuditSecs = 8.0; // s — backoff cadence for hops at/beyond a confirmed loop boundary (user-specified 5-10s range)

// --- Direct-echo hop measurement (the fix for "intermediate hops show loss") -
// A traceroute-style probe measures hop h by sending an Echo to the DESTINATION
// with TTL=h, forcing hop h to emit an ICMP Time-Exceeded. Routers rate-limit
// Time-Exceeded generation HARD (it's control-plane work), so a perfectly
// healthy hop — e.g. an ISP BNG that answers a standalone `ping` at 0% loss —
// paints 30-100% loss under traceroute probing. The final destination, probed
// with a full-TTL Echo that yields a (non-rate-limited) Echo *Reply*, stays at
// 0%: the classic "loss at an intermediate hop, none at the end" artifact.
//
// So once a hop's IP is DISCOVERED, we stop eliciting its Time-Exceeded and
// instead ping that hop's own IP directly with a full-TTL Echo Request —
// exactly what `ping <hop>` does. Its Echo Reply isn't rate-limited, so an
// echo-responsive hop reads ~0% loss for 1 target or 100. A hop that answered
// discovery but does NOT answer a direct ping is detected after kEchoTestTries
// misses and falls back to TTL-limited probing (its rate-limit loss is then
// unavoidable — the same thing traceroute/mtr show for such a hop).
constexpr uint8_t kDirectEchoTtl = 255; // max TTL for a direct hop ping — maximum survivability, always valid regardless of real path length
constexpr int     kEchoTestTries = 4;  // direct misses (with no direct reply yet) before a hop is deemed echo-silent

// Once a hop switches to direct-echo, its own TTL-elicited Time-Exceeded is no
// longer probed each round — so a mid-session route change (a different device
// now answers at this position, e.g. after an ISP-side reroute) would otherwise
// go unnoticed forever: we'd just keep pinging the OLD device's IP directly.
// Every kHopRecheckSecs we send one legacy TTL-limited probe instead, which
// re-confirms (or updates) the hop's real address, exactly like the original
// discovery probe. This is deliberately rare — even summed across 100 targets
// sharing a hop it adds well under 3 probes/sec — and, unlike the normal
// per-interval measurement, it is NOT satisfied by shared-hop adoption, since
// its whole point is to independently re-verify what's actually there.
constexpr double kHopRecheckSecs = 45.0;

// Frankenstein-route guard (see the legacy_miss comment further down, near
// Session::run()'s state variables) — how many consecutive LEGACY-probe
// misses, and how much wall-clock time they must span, before a hop's stale
// address is wiped and rediscovered from scratch. Time-boxed rather than a
// bare count: an echo_ok/probationary hop only gets ONE legacy probe per
// kHopRecheckSecs (so 2 of those, ~90s apart, really is a route-change
// signal) — but an echo_silent hop gets a legacy probe EVERY probe interval
// instead (that's its whole steady-state measurement channel), where 2
// consecutive per-second misses is just ordinary loss/rate-limiting, not a
// route change. Requiring the streak to ALSO span kLegacyMissWindowSecs of
// wall clock keeps the rare-recheck case exactly as sensitive as before
// while no longer mistaking an echo_silent hop's routine jitter for a route
// change — see the timeout-sweep use of these below for the full mechanism.
constexpr int    kLegacyMissThreshold = 2;
constexpr double kLegacyMissWindowSecs = kHopRecheckSecs;

// Residual gap in the time-box above: it protects against a FAST, continuous
// silence stream (echo_silent hops) but does nothing for a hop on the SLOW
// due_recheck channel (echo_ok/probationary hops, one legacy probe per
// kHopRecheckSecs) whose reply rate on THAT specific channel is just
// chronically bad — e.g. a router that answers direct-echo fine but rarely
// bothers generating Time-Exceeded at all. For such a hop, two ~45s-apart
// recheck misses isn't rare — it's business as usual — and wiping it every
// time produces the exact "IP keeps disappearing and reappearing" flapping
// this constant exists to stop.
//
// The actual danger the whole guarded wipe defends against is a hop that
// LOOKS reliable (low loss, believable) silently becoming wrong — a hop
// already showing heavy loss isn't "falsely healthy" in the first place
// (its loss% is already honestly telling the user something's off), so
// there's much less harm in leaving its last-known address in place a
// while longer than in wiping and rediscovering it every ~90s. Gating the
// wipe on the hop's own recent aggregate loss (across BOTH its measurement
// channels, direct and legacy — see HopStats::compute) targets exactly the
// case that matters: a hop that WAS reliable going dark is a real signal;
// a hop that's ALWAYS been unreliable having one more bad patch is not.
constexpr double kGuardedWipeMaxRecentLossPct = 40.0;
constexpr double kGuardedWipeRecentLossWindowSecs = 120.0;

// Second-order guard on top of the recent-loss check above: samples_ gets
// cleared BY every wipe (clear_address()), so "recent loss" alone resets to
// a clean slate each time — a chronically bad hop can pass it again right
// after being rediscovered, simply because rediscovery itself requires a
// handful of lucky replies. Capping how many times the SAME device can be
// wiped (see wipe_count's doc comment above) is what actually stops the
// repeating cycle instead of merely slowing it down. 1 is deliberately
// strict: a real silent reroute only needs to be caught once to fix the
// display; a hop that qualifies for the guarded wipe a SECOND time despite
// that is demonstrating a pattern, not a one-off event.
constexpr int kMaxGuardedWipesPerHop = 1;

// --- Dead-socket / dead-route self-heal (sleep/wake, interface flap) -------
// Complements the Manager-level interface-change watchdog (manager.hpp),
// which reacts to the OS reporting a DIFFERENT interface/address set — the
// fast, common signal for an actual sleep/wake. These two constants are the
// fallback for when the interface list looks unchanged but the network
// still isn't usable yet: send() itself failing repeatedly
// (kMaxConsecutiveSendFails), or send() reporting success while nothing
// ever comes back at all (kSilenceRebuild*) — which is exactly what happens
// if the OS accepts a raw sendto() before its own routing table has caught
// up after a sleep/resume cycle.
constexpr int    kMaxConsecutiveSendFails = 10;
constexpr double kSilenceRebuildMinSecs = 15.0;     // never trigger sooner than this, however fast the probe interval
constexpr double kSilenceRebuildIntervalMult = 5.0; // …or this many probe intervals, whichever is larger
// Session-wide silence (last_any_reply_at, above) misses one real shape of
// stuck target entirely: early hops (e.g. your own router, ISP backbone)
// keep answering fine forever, while the destination — or everything past
// some hop — has been completely dead for a long time. last_any_reply_at
// stays fresh forever in that case (it updates on ANY hop's reply), so the
// watchdog above never fires no matter how long the destination's been
// silent. This second, destination-specific check closes that gap. Its
// threshold is deliberately much longer than the total-silence one: total
// silence across every hop at once is already a strong, near-unambiguous
// signal (would take an actual dead socket/route), but a single
// destination going quiet for a while can just be ordinary loss on a lossy
// link — this needs enough consecutive misses that "genuinely stopped
// responding" is the only realistic explanation left, not "unlucky streak".
constexpr double kDestSilenceRebuildMinSecs = 60.0;
constexpr double kDestSilenceRebuildIntervalMult = 30.0;

// Declared in session.hpp (external linkage, not in an anonymous namespace)
// specifically so tests/test_core.cpp can call it directly without a live
// socket — see the doc comment there for the full contract.
// Shared diagnostic text for "probes go out, but no ICMP error ever comes
// back". This is the single most common cause of TCP/UDP traceroute
// appearing broken on Windows, and it is a platform limitation rather than
// an app bug: a raw ICMP socket on Windows receives ICMP addressed to this
// host, but the stack does NOT hand it ICMP *error* messages (Time-Exceeded,
// Port-Unreachable) that were generated in response to packets sent from a
// DIFFERENT socket. TCP and UDP probes leave via their own sockets, so the
// router's Time-Exceeded is consumed by the TCP/UDP stack and never reaches
// the raw ICMP socket this engine listens on. ICMP mode is unaffected
// because its echo replies belong to that same raw socket.
//
// This is exactly why mature Windows traceroute tools (PingPlotter, nmap,
// tcptraceroute) require a packet-capture driver such as Npcap for TCP/UDP
// tracing — the capture driver sees the ICMP error on the wire regardless of
// which socket "owns" it. On Linux/macOS a raw ICMP socket does receive these
// errors, which is why the same code path works there unmodified.
static std::string icmp_error_delivery_hint(const char* proto_name) {
    const bool elevated = process_is_elevated();
    std::string s = "Probes are being sent, but no ICMP replies from intermediate hops are reaching this app. ";
    if (!elevated) {
        s += "This process is NOT running elevated. Receiving the ICMP Time-Exceeded/Unreachable messages that "
             "hop discovery depends on generally requires Administrator on Windows (root elsewhere) — other "
             "traceroute tools document the same requirement for UDP-style tracing. Try running elevated first; "
             "that is the single most likely fix. ";
    } else {
        s += "This process IS running elevated, so a missing privilege is not the explanation. ";
    }
    s += std::string("If it still fails while ICMP mode works, the path may simply not answer ") + proto_name +
         " probes, or (on Windows, for TCP specifically) a packet-capture driver such as Npcap may be needed to "
         "observe the replies — the destination measurement itself remains valid either way. ICMP mode needs "
         "none of this.";
    return s;
}

uint8_t compute_max_hop(std::optional<uint8_t> dest_hop, std::optional<uint8_t> loop_at_hop,
                        uint8_t frontier, uint8_t max_hops) {
    if (dest_hop) return *dest_hop;
    int window_top = loop_at_hop ? (static_cast<int>(*loop_at_hop) + kLoopAuditWindow)
                                  : (static_cast<int>(frontier) + kDiscoveryWindow);
    int capped = std::min<int>(static_cast<int>(max_hops), window_top);
    return static_cast<uint8_t>(capped < 1 ? 1 : capped);
}

double udp_give_up_threshold(double probe_interval, uint8_t max_hops) {
    double sweeps = probe_interval * static_cast<double>(max_hops) * kUdpGiveUpSweeps;
    return (std::max)(kUdpGiveUpMinSecs, sweeps);
}

double silence_backoff_threshold(double base_threshold, int consecutive_silence_rebuilds) {
    if (consecutive_silence_rebuilds <= 0) return base_threshold;
    int capped_count = (std::min)(consecutive_silence_rebuilds, kSilenceBackoffCapAt);
    double scaled = base_threshold * std::pow(kSilenceBackoffMult, capped_count);
    return (std::min)(kSilenceBackoffCapSecs, scaled);
}

Session::Session(uint64_t id, std::string target, Settings settings)
    : id_(id), target_(std::move(target)), settings_(std::move(settings)) {
    // Per-target-unique ICMP identifier. Every raw ICMP socket in the process
    // receives a copy of every reply, so sessions demultiplex by this id; mixing
    // the monotonically-increasing target id_ in keeps ids distinct across
    // concurrent targets (so one target can't match/consume another's replies)
    // while matching the long-proven v19 behaviour.
    icmp_id_ = static_cast<uint16_t>((static_cast<uint64_t>(now_secs())) ^ id_ ^ 0x4242u);
    // A different, independent constant derivation from the same inputs as
    // icmp_id_ (distinct XOR mask) — the two just need to both be stable for
    // the session's lifetime, not related to each other. 0xFFFF is avoided
    // only for tidiness (0 and 0xFFFF are the same one's-complement "zero" so
    // it wouldn't actually break the pinning invariant either way — see the
    // build_echo derivation notes — but a checksum field that's never all-1s
    // looks less like an anomaly to a casual packet-capture read).
    paris_checksum_target_ = static_cast<uint16_t>((static_cast<uint64_t>(now_secs())) ^ id_ ^ 0xC5A5u);
    if (paris_checksum_target_ == 0xFFFFu) paris_checksum_target_ = 0x0000u;
}

uint16_t Session::next_seq() {
    seq_ = static_cast<uint16_t>(seq_ + 1);
    return seq_;
}

void Session::resolve() {
    // Clear any error left over from a PREVIOUS resolve() attempt before
    // trying again — error_ is otherwise never cleared anywhere, so a
    // transient DNS failure (or family-fallback notice) that later
    // resolves cleanly would stay stuck on screen forever, even once
    // dest_/family_ are freshly and correctly populated below. This must
    // run before getaddrinfo(), not after a successful lookup, so the
    // intentional "falling back to IPv4/IPv6" notices set further down in
    // this same function aren't immediately wiped out the moment they're
    // set.
    error_.reset();
    ensure_winsock_ready(); // must run before any Winsock call (see platform.hpp)
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(target_.c_str(), nullptr, &hints, &res);
    if (rc != 0 || !res) {
        error_ = "DNS resolution failed for " + target_;
        return;
    }
    std::optional<std::string> v4, v6;
    for (addrinfo* p = res; p; p = p->ai_next) {
        char ip[INET6_ADDRSTRLEN] = {0};
        if (p->ai_family == AF_INET && !v4) {
            auto* s = reinterpret_cast<sockaddr_in*>(p->ai_addr);
            inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
            v4 = std::string(ip);
        } else if (p->ai_family == AF_INET6 && !v6) {
            auto* s = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
            inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
            v6 = std::string(ip);
        }
    }
    freeaddrinfo(res);

    // Remember both A and AAAA for possible runtime fallback logic.
    resolved_v4_ = v4;
    resolved_v6_ = v6;

    std::optional<std::string> chosen;
    switch (settings_.family) {
        case FamilyPref::V4: chosen = v4; break;
        case FamilyPref::V6: chosen = v6; break;
        case FamilyPref::Auto: chosen = v4 ? v4 : v6; break;
    }
    // If the requested family has no address but the other family does, fall
    // back to it rather than failing the session. Hostnames frequently map
    // to both A and AAAA; a strict failure here causes an immediate session
    // error or false-loss behaviour in some networks. Fall back improves UX
    // and matches common "happy eyeballs" expectations for tooling.
    if (!chosen) {
        if (settings_.family == FamilyPref::V6 && v4) {
            // Wanted v6 but only v4 exists — fallback to v4.
            error_ = "preferred IPv6 address absent; falling back to IPv4 for " + target_;
            chosen = v4;
        } else if (settings_.family == FamilyPref::V4 && v6) {
            error_ = "preferred IPv4 address absent; falling back to IPv6 for " + target_;
            chosen = v6;
        } else {
            error_ = "no address of the requested family for " + target_;
            return;
        }
    }
    dest_ = chosen;
    family_ = (chosen->find(':') != std::string::npos) ? Family::V6 : Family::V4;
}

void Session::run(std::atomic<bool>* stop, std::atomic<bool>* paused,
                  const std::function<void(const Snapshot&)>& on_update) {
    Protocol proto = settings_snapshot().protocol;
    if (proto == Protocol::Udp) {
        run_udp(stop, paused, on_update);
        return;
    }
    if (proto == Protocol::Tcp) {
        run_tcp(stop, paused, on_update);
        return;
    }
    if (proto == Protocol::Http) {
        run_http(stop, paused, on_update);
        return;
    }
    resolve();
    if (!dest_ || !family_) {
        on_update(snapshot(false, {}));
        return;
    }
    on_update(snapshot(true, {})); // immediate "tracing…" with resolved IP
    register_icmp_owner(icmp_id_, this); // enable cross-session reply routing
    PacerMembership pacer_membership; // count this target toward the global pacer's scaled rate (RAII: leaves on any return)
    g_rdns().ensure_started();        // spin up the shared reverse-DNS resolver on first target
    g_rx().ensure_started();          // spin up the shared RX dispatcher on first target (see its class comment)

    // Decide and validate the effective probing family using both DNS results
    // and the local system's usable interfaces. If the user explicitly chose
    // IPv6 but the host has no IPv6 egress, don't start probing — wait and
    // retry (keeps the UI responsive without emitting a burst of timed-out
    // probes that look like 100% loss). For `Auto`, prefer a family that has
    // both a DNS record and a local egress address.
    {
        Settings s = settings_snapshot();
        auto ifs = list_interfaces();
        bool has_local_v4 = false, has_local_v6 = false;
        for (const auto& ni : ifs) {
            if (ni.v6) has_local_v6 = true; else has_local_v4 = true;
        }

        if (s.family == FamilyPref::Auto) {
            // Prefer the family chosen by resolve() unless it's unusable.
            // This preserves the previous Auto behavior of favoring IPv4 for
            // dual-stack hosts, instead of switching to IPv6 simply because a
            // local IPv6 address exists.
            if (family_ == Family::V4 && resolved_v4_ && has_local_v4) {
                family_ = Family::V4; dest_ = resolved_v4_;
            } else if (family_ == Family::V6 && resolved_v6_ && has_local_v6) {
                family_ = Family::V6; dest_ = resolved_v6_;
            } else if (resolved_v4_ && has_local_v4) {
                family_ = Family::V4; dest_ = resolved_v4_;
            } else if (resolved_v6_ && has_local_v6) {
                family_ = Family::V6; dest_ = resolved_v6_;
            } else if (resolved_v4_) {
                family_ = Family::V4; dest_ = resolved_v4_;
            } else if (resolved_v6_) {
                family_ = Family::V6; dest_ = resolved_v6_;
            }
        } else if (s.family == FamilyPref::V6) {
            // Wait for a usable local IPv6 egress rather than probing blindly.
            if (!has_local_v6) {
                error_ = "No local IPv6 egress available — waiting for IPv6 or change family";
                on_update(snapshot(true, {}));
                // Poll for up to 30s for a new interface or a settings change.
                for (int i = 0; i < 30 && !stop->load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    auto s2 = settings_snapshot();
                    if (s2.family != s.family) break; // user changed pref
                    auto ifs2 = list_interfaces();
                    for (const auto& ni : ifs2) if (ni.v6) { has_local_v6 = true; break; }
                    if (has_local_v6) break;
                }
                if (!has_local_v6 && settings_snapshot().family == FamilyPref::V6) {
                    error_ = "No local IPv6 egress detected; aborting probe for " + target_;
                    on_update(snapshot(false, {}));
                    return;
                }
                // Egress came back (or the family pref changed away from V6)
                // within the wait — the "waiting for IPv6" message above is
                // now stale; without this, it would otherwise sit on screen
                // forever even once probing resumes normally.
                if (has_local_v6) error_.reset();
            }
            if (resolved_v6_) { family_ = Family::V6; dest_ = resolved_v6_; }
        } else if (s.family == FamilyPref::V4) {
            if (!has_local_v4) {
                error_ = "No local IPv4 egress available — waiting for IPv4 or change family";
                on_update(snapshot(true, {}));
                for (int i = 0; i < 30 && !stop->load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    auto s2 = settings_snapshot();
                    if (s2.family != s.family) break;
                    auto ifs2 = list_interfaces();
                    for (const auto& ni : ifs2) if (!ni.v6) { has_local_v4 = true; break; }
                    if (has_local_v4) break;
                }
                if (!has_local_v4 && settings_snapshot().family == FamilyPref::V4) {
                    error_ = "No local IPv4 egress detected; aborting probe for " + target_;
                    on_update(snapshot(false, {}));
                    return;
                }
                // Same as the IPv6 branch above — clear the now-stale
                // "waiting for IPv4" message once egress actually recovers.
                if (has_local_v4) error_.reset();
            }
            if (resolved_v4_) { family_ = Family::V4; dest_ = resolved_v4_; }
        }
    }

    // Acquire (never own outright) a SHARED pooled socket for this exact
    // (family, privileged, source_addr) combo — see acquire_pooled_socket's
    // doc comment (transport.hpp) and the RxDispatcher comment above for the
    // full Pillar 1 design. `sock` keeps it alive for as long as this session
    // needs it; releasing/reacquiring (on rebuild, below) works exactly like
    // the old make_prober()/prober did, just shared instead of exclusive.
    auto acquire_sock = [&]() {
        Settings s = settings_snapshot();
        return acquire_pooled_socket(*family_, s.privileged, s.source_addr);
    };
    auto sock = acquire_sock();
    if (!sock->ok()) {
        error_ = sock->error();
        on_update(snapshot(false, {}));
        return;
    }
    // ProbeIcmp adapter around `sock` — see probe.hpp/probe_icmp.hpp's doc
    // comments. Deliberately a thin wrapper only: this refactor's whole
    // point is routing the three real send() call sites below through the
    // ProbeStrategy interface with ZERO behavior change, as the
    // prerequisite for a future protocol to plug in alongside ICMP — not a
    // rewrite of anything ICMP mode already does. Rebuilt alongside `sock`
    // on every rebuild (see the `sock = std::move(new_sock)` site below).
    auto strategy = std::make_unique<ProbeIcmp>(sock, icmp_id_);

    // Continuous (mtr-style) probing: one probe per hop every `interval`, replies
    // matched as they arrive, probes declared lost when they exceed `timeout`,
    // and a snapshot pushed several times a second. This decouples the UI update
    // rate from the timeout, so lossy hops surface quickly and the view is live.
    struct Pending { uint8_t hop; double sent_at; bool direct = false; std::string direct_ip; };
    std::map<uint16_t, Pending> pending;
    std::map<uint8_t, double> next_send;
    std::vector<NewPoint> buffer;
    double last_emit = now_secs();
    double last_rdns = 0.0;             // last time we swept hops for reverse-DNS names
    double dest_found_at = 0.0;         // wall-clock of the FIRST reply from the destination (0 = not yet)
    double tokens = kProbeBurst;        // per-target send-rate token bucket (see kMaxProbeRate)
    double last_refill = now_secs();
    std::map<uint8_t, int> tries;       // per-hop probe attempts (drives fast->steady backoff of non-responders)
    uint8_t rr = 1;                     // round-robin cursor so the paced budget is shared fairly across unanswered hops
    // BUG FIX: the answered-hop keepalive phase below used to iterate a flat
    // 1..max_hop with no rotation, even though the unanswered phase right
    // above it was deliberately made round-robin for precisely this reason
    // (see that loop's own comment). The paced budget is a shared, capped
    // token bucket (GlobalPacer::kBurst is only 8 accumulated tokens), so
    // once several targets on overlapping paths are all competing — or one
    // target simply has more hops than the burst allowance — a flat scan
    // starting at ttl 1 every pass lets the LOW ttls consume every available
    // token and starves the higher ones permanently. Observed live as hop 1
    // accumulating thousands of samples while every hop beyond it sat at
    // sent=0/recv=0 indefinitely, "fixable" only by deleting and re-adding
    // the target (which just reset the contention). Rotating this phase too
    // means no hop can be starved indefinitely: whichever hops miss out on
    // one pass are the ones served first on the next.
    uint8_t rr_ans = 1;                 // round-robin cursor for the answered-hop keepalive phase
    std::map<uint8_t, double> last_reply_at; // per-hop wall-clock of last REAL reply (drives frontier decay + ghost pruning)
    // BUG FIX: see the "Decay the frontier" block's doc comment further
    // down for the mechanism this feeds. That decay logic only had
    // last_reply_at (recency) to work with, so it could not distinguish a
    // single fluke reply (the scenario it was actually written to guard
    // against — its own comment describes exactly that) from a hop that
    // had answered reliably for a long time and simply went quiet through
    // an ordinary gap (sleep/wake being the most common real-world one —
    // any gap past kFrontierStaleSecs, currently a flat 30s, decayed a
    // fully-established path exactly as aggressively as a one-off fluke).
    // Once decayed, recording a hop's timeout results stops entirely (the
    // `bound` check just below the decay block) — not just its DISPLAY,
    // its actual stats freeze at whatever they were and never update
    // again, even though probes keep being sent to it (the SEPARATE
    // frontier that gates the send ceiling itself, a few lines up, does
    // NOT decay — only this timeout-recording gate does), which is what
    // produced the exact reported pattern: real, previously fully-mapped
    // hops stuck showing stale, frozen sent/recv numbers indefinitely
    // after any sufficiently long gap, never recovering even after a
    // rebuild or a manual Force Recheck, because nothing about either of
    // those repairs this decay decision on its own. A hop that has
    // answered at least kEstablishedHopReplies times is presumed genuinely
    // real, not a fluke, and is now exempt from staleness decay entirely —
    // restoring the guard's original documented intent (protect against a
    // single stray reply) without the side effect of also punishing
    // reliable, well-proven hops for an ordinary silence gap.
    std::map<uint8_t, int> reply_count; // per-hop lifetime count of genuine replies THIS session — see above
    static constexpr int kEstablishedHopReplies = 3;
    std::map<uint8_t, bool> echo_ok;         // hop answered a DIRECT ping at least once → measure it by direct echo (rate-limit-free)
    std::map<uint8_t, bool> echo_silent;     // hop answered discovery but never a direct ping → fall back to TTL-limited probing
    std::map<uint8_t, int>  echo_miss;       // consecutive direct-ping misses while still unconfirmed (drives the echo_silent decision)
    std::map<uint8_t, double> hop_recheck_at; // last time this hop got a legacy TTL-elicited re-confirm probe (see kHopRecheckSecs)
    // Consecutive LEGACY (non-direct, TTL-elicited) probe timeouts for a hop
    // that already has an address on file. Distinct from echo_miss above,
    // which only tracks DIRECT-echo misses and drives fallback to legacy
    // probing — this tracks the opposite direction: a legacy probe timing
    // out. On a route change, if the new device at this hop replies (even
    // with a different address), set_address() already handles it correctly
    // via the flap path. If the new device is silent instead (drops the
    // legacy probe without replying), NOTHING previously cleared the old
    // address — so a separate DIRECT-echo probe to that old, now-wrong,
    // still-globally-routable address could keep getting real replies from
    // the device that isn't on this path anymore, producing a hybrid/
    // "Frankenstein" hop that blends two different physical routes. See the
    // legacy-miss handling in the timeout sweep below.
    std::map<uint8_t, int> legacy_miss;
    // Wall-clock of the FIRST miss in the current legacy_miss streak for this
    // hop — reset (erased) the moment the streak breaks (any successful
    // legacy reply) or the hop's address changes/clears. Paired with
    // legacy_miss[hop] to decide the guarded wipe below: the streak must
    // both reach kLegacyMissThreshold misses AND span kLegacyMissWindowSecs
    // of real time, so a burst of sub-second misses on a fast, continuously
    // legacy-probed (echo_silent) hop can't trigger it the way a genuine
    // ~45s-apart recheck failure can — see kLegacyMissWindowSecs above.
    std::map<uint8_t, double> legacy_miss_since;
    // How many times THIS device (not this TTL slot — see the flap-reset
    // note below) has been through the guarded wipe below. Deliberately NOT
    // cleared when a wipe happens (unlike legacy_miss/legacy_miss_since) —
    // it exists specifically to survive the wipe it's counting, because the
    // recent-loss gate (kGuardedWipeMaxRecentLossPct) alone isn't enough: a
    // hop's samples_ get cleared BY the wipe, so its "recent loss" reads
    // artificially good again immediately after rediscovery (whatever
    // handful of replies it takes to get rediscovered are, almost by
    // definition, a lucky streak) — long enough for a genuinely chronic hop
    // to pass the recent-loss check again and get wiped a second time, a
    // third, forever, which is exactly the repeating "IP disappears and
    // reappears" cycle a real user would notice. Only reset on a CONFIRMED
    // flap to a genuinely different device (see the flap-detection block
    // below) — a new device deserves its own fresh allowance; the same
    // device coming back after being wiped does not.
    std::map<uint8_t, int> wipe_count;

    // Consecutive raw send() failures (see try_send below) — NOT the same as
    // a probe timing out (that's a real send that got no reply); this is the
    // OS refusing/failing the sendto() call itself. A stale socket left over
    // from before a sleep/interface change is the classic cause. After
    // kMaxConsecutiveSendFails in a row, request a socket rebuild so the
    // engine heals itself instead of silently spinning forever refunding
    // pacer tokens — see the try_send lambda for where this increments.
    int consecutive_send_fails = 0;

    // Wall-clock of the most recent REAL reply this session received, of any
    // kind (direct-echo, legacy/TTL-elicited, or shared-hop adoption) — a
    // session-wide signal, deliberately not scoped to one hop. If sends are
    // going out but ABSOLUTELY NOTHING has come back for kSilenceRebuild*
    // (see above), that's very unlikely to be ordinary per-hop loss (it
    // would need every single hop, including the destination, to go silent
    // at once) and much more likely a dead socket/route that never actually
    // errors on send() — e.g. the OS accepting a raw sendto() before its
    // routing table has caught up after a sleep/resume cycle, which a
    // send-failure counter alone would never catch. Reset on every reply and
    // whenever the loop is paused (see the paused branch below), so the
    // clock doesn't run while there's nothing to receive by design.
    double last_any_reply_at = now_secs();
    // Consecutive silence-triggered rebuilds since the last GENUINE reply
    // (any kind — direct echo, legacy/TTL-elicited, or shared-hop adoption;
    // reset at the same three sites last_any_reply_at itself is reset by a
    // real reply, deliberately NOT at the rebuild-success reset just above
    // this comment block's own last_any_reply_at reset, since a socket
    // rebuild completing is not evidence the underlying block cleared — see
    // silence_backoff_threshold's doc comment, session.hpp). Feeds section
    // 3b below.
    int consecutive_silence_rebuilds = 0;
    // BUG FIX: this used to be the SAME counter 3b uses above, on the
    // stated reasoning that "either trigger means the same thing: another
    // rebuild that didn't restore real replies." That reasoning doesn't
    // hold: 3c's own condition is specifically "the destination is silent
    // while OTHER hops keep answering" — hop 1 in particular is often the
    // local router, replying every probe_interval indefinitely. Sharing
    // the counter meant hop 1's own constant, healthy traffic reset it to
    // 0 on essentially every reply cycle (session-wide resets are
    // unconditional on which hop replied), so 3c's backoff could never
    // accumulate past its first step no matter how long the destination
    // itself stayed silent — exactly the "hundreds of automatic reconnect
    // attempts, still no reply in 1h" pattern a live report showed even
    // with 3b's backoff already in place and working correctly for its
    // own (genuinely-all-silent) case. This counter now only resets when
    // the DESTINATION hop specifically replies (see the three reply sites
    // below) — proof that THIS particular problem cleared, not that some
    // unrelated hop is still fine.
    int consecutive_dest_silence_rebuilds = 0;

    // Routing-loop auditor state (see the comment block above near
    // kLoopAuditSecs for the full rationale). loop_at_hop: the confirmed loop
    // boundary, if any — compute_max_hop() freezes the discovery window here.
    // loop_confirm: two-strikes counter per (higher hop, lower/duplicated
    // hop) pair — a single sighting is not enough to freeze (see the ECMP
    // false-positive discussion above); it takes two independent replies
    // confirming the same pair. The advisory itself lives in loop_warning_/
    // loop_hop_/loop_dup_at_ (session.hpp) — dedicated fields, not shared
    // with error_, so no loop_error flag is needed to track "whose message
    // is this" anymore.
    std::optional<uint8_t> loop_at_hop;
    std::map<std::pair<uint8_t, uint8_t>, int> loop_confirm;

    auto ensure_hop = [&](uint8_t h) -> HopStats& {
        auto it = hops_.find(h);
        if (it == hops_.end()) it = hops_.emplace(h, HopStats(id_, h)).first;
        return it->second;
    };
    // Predecessor address for the shared-hop cache key (see the SharedHopTable
    // comment above for why the edge, not just the node, is what's keyed on).
    // The predecessor is simply hop-1 — NOT the nearest already-resolved hop
    // further back. Using the nearest resolved hop as a stand-in collapses
    // every target whose immediate predecessor happens to be unresolved onto
    // whichever earlier hop DID resolve, even though two targets can diverge
    // at that unresolved hop and reach genuinely different next hops — they'd
    // wrongly share one cache entry and stomp each other's real measurements.
    // If hop-1 itself hasn't resolved, that's still a distinct, stable key —
    // distinct per missing TTL (not the single shared "SRC" sentinel), so two
    // targets that both have an unresolved hop-1 (for unrelated reasons, or
    // because they've genuinely diverged before hop 1 resolves) don't collide
    // either.
    auto predecessor_of = [&](uint8_t hop) -> std::string {
        if (hop <= 1) return shared_hop_src_sentinel();
        uint8_t h = hop - 1;
        auto it = hops_.find(h);
        if (it != hops_.end() && it->second.address()) return *it->second.address();
        return "UNKN-" + std::to_string(h);
    };

    while (!stop->load()) {
        // Apply any live config change (interface/family rebuild the socket).
        bool rebuild = false;
        {
            std::lock_guard<std::mutex> lk(settings_mtx_);
            if (rebuild_needed_) rebuild = true;
            settings_dirty_ = false;
        }
        // Checked BEFORE the routine `rebuild` trigger below (this used
        // to be the other way around, as an `else if`) for a concrete
        // reason: a session stuck in a repeating automatic-rebuild cycle
        // (the silence watchdog re-triggering every pass, or a
        // chronically flapping interface being reported by the
        // Manager-level network watchdog) previously meant `rebuild` was
        // true on nearly every iteration, and this branch, being an
        // `else if`, never got a turn: force_recheck_needed_ would sit
        // set, un-consumed, indefinitely, because the loop never reached
        // the code that reads it. That is exactly backwards — a session
        // wedged in an active rebuild storm is precisely the situation
        // where a user reaching for "Force recheck" most needs it to
        // actually do something, and a manual, user-initiated request
        // should never be silently starved by an automatic one it did
        // not even know was happening. This branch's own body sets
        // rebuild_needed_ = true itself regardless (see below), so
        // nothing about the routine rebuild's own effect is lost by
        // giving this priority — it still happens, just on the very next
        // pass instead of whichever pass happens to find `rebuild`
        // momentarily false.
        if (force_recheck_needed_.exchange(false)) {
            // Also force a fresh socket, every time — see the doc comment on
            // Snapshot::silence_rebuild_count in session.hpp and the
            // destination-specific silence check (3c) further down in this
            // file: a target can sit with SOME hops (e.g. 1..5)
            // still answering fine while everything past a given hop has
            // been completely dead for a long time, and the automatic
            // silence watchdog below only ever looks at session-wide
            // last_any_reply_at — it will never fire for that shape of
            // failure, because the early hops keep it "not silent" forever.
            // Previously Force Recheck's only effect was re-arming the SAME
            // discovery cadence that's already been running and failing on
            // its own — clicking it just restarted an already-failing cycle
            // sooner, with nothing actually different about the next
            // attempt. A fresh socket (new local port, new ICMP identifier)
            // is the one thing that's never been tried automatically for
            // this shape of stuck target, and is what "do everything initial
            // discovery does" concretely means at the transport layer. Not
            // gated on route_context_changed, so this alone does NOT wipe
            // hops_/dest_hop_ — an already-good hop's data survives exactly
            // as before; only the socket underneath it is replaced.
            {
                std::lock_guard<std::mutex> lk(settings_mtx_);
                rebuild_needed_ = true;
            }
            // User-requested recheck. First, cheaply check whether the
            // DESTINATION's own DNS answer has changed — resolve() alone
            // never mutates anything the UI shows, it only fills dest_/
            // family_/resolved_v4_/resolved_v6_, so calling it here to
            // compare is free of side effects unless something really
            // changed. This is what a hostname target losing its IP over a
            // sleep/resume cycle (a genuinely stale DNS answer, not just a
            // stale socket) needs — previously Force Recheck never called
            // resolve() at all, so a target stuck this way had no button
            // that could fix it. If the answer changed (or resolution
            // starts failing), route this through the SAME rebuild path
            // settings changes already use (below) rather than duplicating
            // its socket/state-reset logic here — `continue` so this pass
            // re-enters the loop and the `rebuild` branch handles it.
            {
                auto old_dest = dest_;
                resolve();
                if (!dest_ || dest_ != old_dest) {
                    std::lock_guard<std::mutex> lk(settings_mtx_);
                    rebuild_needed_ = true;
                    continue;
                }
            }
            // DNS unchanged — fire the SAME automatic route-update mechanism
            // that already runs every kHopRecheckSecs (see due_recheck in
            // try_send below) right now, instead of waiting out the timer —
            // no separate window, no separate retry path, and no wipe of any
            // hop that currently HAS an address. Zeroing hop_recheck_at makes
            // due_recheck true on the very next pass for every already-known
            // hop, which is exactly what the periodic timer does on its own
            // cadence; hops_/last_reply_at/dest_hop_ are left untouched, so
            // an already-addressed hop only ever gets an in-place update (its
            // address/RTT changing), never a reset. A hop that's currently
            // BLANK is different — see the else branch below — since there's
            // no existing measurement there to disturb, only its discovery
            // cadence is re-armed.
            for (auto& [ttl, hs] : hops_) {
                if (hs.address()) {
                    hop_recheck_at[ttl] = 0.0;
                } else {
                    // A hop showing blank (`*`) — either still mid-discovery,
                    // or it lost its address earlier (e.g. the legacy-miss
                    // guarded wipe, kLegacyMissThreshold above) and has since
                    // fallen back to the slow steady `interval` cadence for
                    // rediscovery (see kDiscoveryTries' fast-cadence comment
                    // in try_send). A recheck request is exactly the moment
                    // to give it a fresh fast-discovery burst on demand
                    // instead of making the user wait out however long the
                    // steady cadence takes to get lucky against a router that
                    // only rarely answers a TTL-elicited probe — same
                    // kDiscoveryTries cap either way, just re-armed now.
                    tries[ttl] = 0;
                    next_send[ttl] = 0.0;
                }
            }
            // Also immediately re-arm the destination-shrink probe (see
            // dest_shrink_check_at_ below), which likewise already runs on
            // its own kHopRecheckSecs cadence — a manual recheck should check
            // for a route that's gotten SHORTER just as eagerly as it
            // re-verifies already-known hops.
            dest_shrink_check_at_ = 0.0;
        } else if (rebuild) {
            // BUG FIX: this counter was published to the UI (as
            // silenceRebuildCount) to answer "is this session actively
            // retrying right now" — its own original doc comment said so
            // — but it only ever incremented and was never reset anywhere.
            // A session that briefly lost ICMP (e.g. a VPN toggle) and then
            // fully recovered kept this pinned at whatever number it reached
            // during the outage, FOREVER, producing exactly what a live
            // report showed: "4 automatic reconnect attempts, still no reply
            // in 55m ago" displayed over hop data that was healthy across all
            // 13 hops with sub-30ms RTT — stale, contradictory, and permanent
            // once triggered. See the four reset sites (search this file for
            // "real reply — clears the reconnect-attempts banner too") for
            // where it now goes back to 0, mirroring exactly the conditions
            // that already reset consecutive_silence_rebuilds/
            // consecutive_dest_silence_rebuilds for the same reason.
            ++silence_rebuild_count_;
            // Attempt to re-resolve and re-acquire a pooled socket for the
            // (possibly new) family/source/privileged combo. If this
            // transiently fails (DNS or socket), keep the session running
            // (don't return) so the UI spinner and discovery state remain
            // visible; retry shortly. Dropping the old `sock` shared_ptr here
            // (via reassignment below) releases this session's share of the
            // OLD pooled socket — if no other session still uses that exact
            // combo, the pool's entry for it goes stale and its underlying
            // socket closes, same lifecycle as the old make_prober() had, just
            // shared instead of exclusive.
            //
            // rebuild_needed_ is deliberately NOT cleared until a rebuild
            // actually succeeds (see the success branch below) — an interface
            // that's still coming up (DNS not ready yet, or the new socket
            // fails to bind) must keep retrying every pass instead of getting
            // exactly one attempt and then being stuck on whatever `sock` that
            // one attempt left behind (a still-good old socket dropped for a
            // broken new one, or an untouched old one nobody frees) until the
            // user happens to touch settings again.
            //
            // Captured BEFORE resolve() overwrites dest_/family_ — compared
            // against their post-resolve values further down to decide
            // whether this rebuild represents a genuine route change (wipe
            // accumulated history) or just a local socket rebuild (don't).
            std::optional<std::string> dest_before_rebuild = dest_;
            std::optional<Family> family_before_rebuild = family_;
            resolve(); // family may have changed → re-pick dest/family
            if (!dest_ || !family_) {
                // leave the previous state intact, surface a running snapshot
                on_update(snapshot(true, {}));
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            } else {
                auto new_sock = acquire_sock();
                if (!new_sock->ok()) {
                    error_ = new_sock->error();
                    on_update(snapshot(true, {}));
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                } else {
                    sock = std::move(new_sock);
                    strategy = std::make_unique<ProbeIcmp>(sock, icmp_id_); // rebuilt alongside sock — see its construction site's doc comment
                    {
                        std::lock_guard<std::mutex> lk(settings_mtx_);
                        rebuild_needed_ = false;
                    }
                    // Always necessary regardless of what changed: probes
                    // in flight were sent on the OLD socket, so their
                    // replies (if any still arrive) can't be meaningfully
                    // matched against anything; the rate limiter and
                    // self-heal counters are cheap to just restart clean.
                    // A successful rebuild means we just got a working
                    // socket (and, above, a fresh resolve()) — whatever
                    // caused the dead-socket/silence symptoms is resolved,
                    // so both self-heal counters restart clean instead of
                    // possibly re-triggering another rebuild on stale counts.
                    pending.clear();
                    next_send.clear();
                    tokens = kProbeBurst;
                    last_refill = now_secs();
                    consecutive_send_fails = 0;
                    last_any_reply_at = now_secs();

                    // Only wipe accumulated per-hop history (hops_, discovery
                    // progress, loop-detection state — everything below) if
                    // the ROUTE itself actually changed: a different resolved
                    // destination address, or switching address family. Those
                    // hops would genuinely be different devices, so the old
                    // data is meaningless. A rebuild triggered by nothing more
                    // than the LOCAL egress interface changing (e.g. Windows
                    // failing Ethernet over to Wi-Fi and back within a few
                    // seconds) still reaches the exact same destination, very
                    // often via a substantially unchanged path beyond the
                    // local gateway — wiping months of history and the whole
                    // graph for a brief local link blip throws away exactly
                    // the continuity a monitoring tool exists to provide.
                    // hops_'s in-memory stats are the hot tier of a
                    // deliberately hybrid store (see ColdStore/compute_stat_
                    // tiered in manager.hpp) specifically so RAM doesn't have
                    // to hold unbounded history — leaving hops_ alone here
                    // means that relationship, and everything the user has
                    // already been shown for this session, survives a local
                    // link flap intact. If a hop genuinely IS a different
                    // device after all this, the existing flap/loop-detection
                    // machinery (§4/§5, ARCHITECTURE.md) is exactly what's
                    // supposed to catch and correct that at the per-hop
                    // level — that machinery doesn't need a full session
                    // wipe to do its job.
                    bool route_context_changed =
                        (dest_ != dest_before_rebuild) || (family_ != family_before_rebuild);
                    if (route_context_changed) {
                        dest_hop_.reset();
                        max_hop_seen_ = 0;
                        hops_.clear();
                        dest_found_at = 0.0;   // discovery restarts from scratch
                        tries.clear();
                        rr = 1;
                        last_reply_at.clear();
                        echo_ok.clear();
                        echo_silent.clear();
                        echo_miss.clear();
                        legacy_miss.clear();
                        legacy_miss_since.clear();
                        wipe_count.clear();
                        hop_recheck_at.clear();
                        loop_at_hop.reset();
                        loop_confirm.clear();
                        loop_warning_.reset();
                        loop_hop_.reset();
                        loop_dup_at_.reset();
                    }
                }
            }
        }

        if (paused && paused->load()) {
            // Keep the silence-watchdog clock from running while there's
            // nothing to receive by design — otherwise resuming after a long
            // pause would immediately look like a dead socket and force an
            // unnecessary rebuild.
            last_any_reply_at = now_secs();
            consecutive_silence_rebuilds = 0; // resuming after a pause shouldn't inherit backoff accrued before it
            silence_rebuild_count_ = 0; // same reasoning — a manual pause/resume shouldn't leave a stale reconnect-attempts count behind either
            consecutive_dest_silence_rebuilds = 0; // same reasoning — see its own doc comment
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        Settings s = settings_snapshot();
        double interval = s.probe_interval > 0.01 ? s.probe_interval : 1.0;
        // timeout = 0 means "auto": wait at least the interval (and >= 1s) before
        // declaring loss — like ping/PingPlotter, which expose interval, not a
        // hard timeout.
        double timeout = s.timeout > 0.0 ? s.timeout : std::max(interval, 1.0);
        // Probe range. Once the destination is known we probe exactly 1..dest.
        // While still discovering, use a sliding window (see kDiscoveryWindow
        // in session.hpp): only probe up to kDiscoveryWindow hops past the
        // deepest already-resolved hop, so we walk out to the destination
        // instead of blasting every TTL at it at once (which is what a
        // rate-limiting IPv6 endpoint drops en masse) — UNLESS a routing loop
        // is confirmed (loop_at_hop), in which case the window instead freezes
        // at the loop boundary (see the loop-auditor comment near
        // kLoopAuditSecs, and compute_max_hop() in session.hpp).
        uint8_t frontier = 0;
        for (const auto& [h, cnt] : tries) {
            bool resolved = last_reply_at.count(h) > 0 || cnt >= kFrontierProbes;
            if (resolved && h > frontier) frontier = h;
        }
        uint8_t max_hop = compute_max_hop(dest_hop_, loop_at_hop, frontier, s.max_hops);
        double now = now_secs();

        // Per-target token-bucket pacer. Refill toward kMaxProbeRate (capped at
        // a small burst) and charge one token per actual send below. This makes
        // THIS target's ICMP send rate a smooth, constant stream that can never
        // exceed kMaxProbeRate — no matter how many hops are "due" — which is
        // what keeps fast discovery from looking like an ICMP flood/scan to AV.
        // The process-wide g_pacer (top of file) additionally bounds the sum of
        // all targets; a send must satisfy both.
        tokens = (std::min)(kProbeBurst, tokens + (now - last_refill) * kMaxProbeRate);
        last_refill = now;

        bool have_dest = dest_hop_.has_value();
        // Brief settle window right after the destination first answers, so its
        // kDiscoveryDropCount settling replies clear quickly instead of taking
        // kDiscoveryDropCount·interval. Actual sends stay paced by the bucket.
        bool settling = have_dest && (now - dest_found_at) < kDestSettleSecs;
        bool discovering = !have_dest || settling;

        // A hop is "answered" once any reply (echo or TTL-exceeded) has given it
        // an address; those don't need fast re-probing to be found.
        auto answered = [&](uint8_t ttl) -> bool {
            auto it = hops_.find(ttl);
            return it != hops_.end() && it->second.address().has_value();
        };

        // 1) send due probes, paced by the token bucket
        double soonest = now + 0.25;
        bool due_but_paced_out = false;
        auto try_send = [&](uint8_t ttl) {
            if (s.paused_hops.count(ttl)) return; // user paused this hop
            double& nx = next_send[ttl];
            // First-probe stagger (as before): ramp across hops instead of one
            // simultaneous burst. Subsequent cadence is carried by `nx`.
            if (nx == 0.0) nx = now + (ttl - 1) * kSendStaggerSecs;
            if (now >= nx) {
                bool have_addr = answered(ttl);
                std::string hip;
                if (have_addr) hip = *hops_.find(ttl)->second.address();

                // Periodic re-discovery: even for an already-known, direct-echo
                // hop, occasionally send ONE legacy TTL-elicited probe so a
                // mid-session route change (a different device now answers at
                // this position) is noticed instead of silently pinging a
                // stale IP forever (see kHopRecheckSecs). This must NOT be
                // satisfied by adoption below — its whole point is to
                // independently re-verify what's actually there. (Previously
                // this had a "shared-recheck relief" path that let another
                // target sharing the same edge satisfy this instead — removed:
                // it contradicted this exact design decision. See
                // ARCHITECTURE.md §3's explicit "never satisfied by shared-hop
                // adoption... its whole point is independent re-verification",
                // and §5's checksum-pinning discussion for why: ECMP hashing
                // typically includes the destination IP, so two different
                // targets sharing an observed predecessor can still
                // legitimately diverge onto different branches at or beyond
                // this hop — checksum pinning only fixes WITHIN-session
                // path variance, not this cross-target case. Ordinary
                // adoption already accepts that risk for routine RTT
                // measurement, where a wrong sample self-corrects within
                // about one interval; letting it also satisfy IDENTITY
                // reconfirmation risks a wrong address lingering, and
                // cascading through other targets' own relief checks.)
                double& recheck_at = hop_recheck_at[ttl];
                bool due_recheck = have_addr && (now - recheck_at >= kHopRecheckSecs);

                // A confirmed routing loop (see the loop-auditor comment near
                // kLoopAuditSecs) forces this hop into slow, legacy-only,
                // never-adopted probing — covers both the loop boundary hop
                // itself and the one audit hop past it (the only two
                // positions compute_max_hop() keeps in scope while a loop is
                // active). Never a hard stop: this hop keeps getting a real
                // probe every kLoopAuditSecs, so a recovery is always noticed.
                bool loop_audit = loop_at_hop && ttl >= *loop_at_hop;

                // Shared-hop adoption: if another target on the same egress
                // already has a fresh real reply for this exact hop IP, adopt
                // it as our own sample and DON'T send. This is what stops N
                // targets from each probing the shared router/BNG every
                // interval — the hop sees ~one probe total instead of N.
                bool adopted = false;
                if (have_addr && !due_recheck && !loop_audit) {
                    // Freshness bound: a shared sample older than ~1.5 intervals
                    // is ignored, so if the owner stops answering (hop actually
                    // dropping) we fall back to real probing within one interval
                    // and surface the true loss — self-healing, not sticky.
                    // Keyed on the edge (predecessor -> this hop's IP), not just
                    // the IP, so two targets that reach the same public node via
                    // different upstream paths get independent measurements
                    // instead of silently overwriting each other's real numbers.
                    auto shared = shared_adopt(s.source_addr, predecessor_of(ttl), hip, id_, now, interval * 1.5 + 0.05);
                    if (shared) {
                        hops_.find(ttl)->second.push(now, *shared);
                        buffer.push_back(NewPoint{ttl, now, *shared});
                        last_reply_at[ttl] = now;
                        last_any_reply_at = now; // session-wide: see the silence-watchdog comment above — a fresh adopted sample is still real evidence this host's network stack is working
                        consecutive_silence_rebuilds = 0; // real evidence, not just a rebuilt socket — see its own doc comment
                        silence_rebuild_count_ = 0; // real reply — clears the reconnect-attempts banner too, see its own doc comment
                        if (dest_hop_ && ttl == *dest_hop_) consecutive_dest_silence_rebuilds = 0; // see its own doc comment — only THIS hop's reply counts here
                        nx = now + interval; // steady cadence; we measured via adoption
                        adopted = true;
                    }
                }
                if (adopted) { soonest = (std::min)(soonest, nx); return; }

                // Direct-echo measurement (the fix for shared-hop rate-limit
                // loss, see kDirectEchoTtl above): once a hop's IP is known and
                // it hasn't proven echo-silent, ping that IP directly full-TTL
                // instead of eliciting a Time-Exceeded via *dest_. A due
                // recheck, or an active loop audit, temporarily forces the
                // legacy TTL-limited style instead — for loop_audit, this is
                // what stops a confirmed-duplicate hop from opening its own
                // redundant direct-echo stream to the same looping IP another
                // hop (or another session) is already pinging.
                bool use_direct = have_addr && !echo_silent.count(ttl) && !due_recheck && !loop_audit;

                // A real send needs BOTH a per-target token AND a global pacer
                // token, so neither one target nor the whole process can exceed
                // its cap. Take the global token only after the local one is
                // available; refund it if the send itself fails.
                if (tokens >= 1.0 && g_pacer().try_take(now)) {
                    uint16_t seq = next_seq();
                    // pin_checksum only affects IPv4 sends (build_echo ignores
                    // it for V6); passing it unconditionally is harmless and
                    // avoids an extra family branch here.
                    bool sent = use_direct ? strategy->send_direct_probe(hip, seq, s.payload_size, paris_checksum_target_)
                                           : strategy->send_hop_probe(*dest_, ttl, seq, s.payload_size, paris_checksum_target_);
                    if (!sent) {
                        // If send failed and we're unprivileged and using a
                        // large payload, try a conservative fallback once.
                        size_t fallback = 1432;
                        if (!s.privileged && s.payload_size > fallback) {
                            sent = use_direct ? strategy->send_direct_probe(hip, seq, fallback, paris_checksum_target_)
                                              : strategy->send_hop_probe(*dest_, ttl, seq, fallback, paris_checksum_target_);
                        }
                    }
                    if (sent) {
                        pending[seq] = Pending{ttl, now, use_direct, use_direct ? hip : std::string()};
                        tokens -= 1.0;
                        if (!use_direct) ++tries[ttl];
                        if (due_recheck) recheck_at = now;
                        consecutive_send_fails = 0;
                    } else {
                        // do not consume a token or count a try for a failed send;
                        // refund the global token we took, and mark that pacing
                        // prevented sends this round.
                        g_pacer().refund();
                        due_but_paced_out = true;
                        // The raw send() call itself failed (not a timeout —
                        // this is the OS refusing sendto() outright), most
                        // often a socket left over from before a sleep/
                        // interface change. kMaxConsecutiveSendFails in a row
                        // requests a rebuild so the engine heals itself
                        // instead of spinning here forever — see
                        // kMaxConsecutiveSendFails' comment above.
                        if (++consecutive_send_fails >= kMaxConsecutiveSendFails) {
                            std::lock_guard<std::mutex> lk(settings_mtx_);
                            rebuild_needed_ = true;
                            consecutive_send_fails = 0;
                        }
                    }
                    // Fast cadence only where it helps: an answered hop only
                    // during the post-destination settle window; an unanswered
                    // hop only for its first kDiscoveryTries attempts (after
                    // that it's a non-responder — back off to the steady
                    // interval so it stops consuming the paced budget). Every
                    // other case uses the configured interval — EXCEPT a
                    // confirmed loop-audit hop, which always uses the slow
                    // kLoopAuditSecs backoff regardless of any of that, so a
                    // stuck tail costs near-zero load without ever going silent.
                    //
                    // Deliberately NOT gated on the session-wide `discovering`
                    // flag for the unanswered-hop case (unlike the have_addr
                    // branch, which legitimately only wants the settle-window
                    // burst once, right after initial route discovery). That
                    // flag answers "is the SESSION still finding its route",
                    // but a hop can lose its own address well after the
                    // session is fully settled — e.g. the legacy-miss guarded
                    // wipe (see kLegacyMissThreshold above) firing on a
                    // long-running target — and tries[ttl] is already reset
                    // to 0 whenever that happens, so this is exactly the same
                    // per-hop kDiscoveryTries-capped burst, just no longer
                    // blocked from re-firing after the session's very first
                    // discovery phase. Doesn't reopen the flood/scan risk the
                    // `discovering` gate was originally about avoiding (see
                    // the design comment above kDiscoveryInterval): the real
                    // safety net is the per-target token bucket and the
                    // process-wide pacer, both still fully in effect here —
                    // this only changes how favourably a SPARSE, already-
                    // capped burst for one re-lost hop is scheduled within
                    // that budget, not how large the budget itself is.
                    if (loop_audit) {
                        nx = now + kLoopAuditSecs;
                    } else {
                        bool fast = have_addr ? settling
                                              : (tries[ttl] < kDiscoveryTries);
                        nx = now + (fast ? (std::min)(interval, kDiscoveryInterval) : interval);
                    }
                } else {
                    due_but_paced_out = true; // no token this round; retry once one refills
                }
            }
            soonest = (std::min)(soonest, nx);
        };
        // Priority: hops we haven't heard from first (any TTL >= the
        // destination's distance reaches the destination itself, so this is what
        // finds the route) — visited ROUND-ROBIN so the shared, rate-capped
        // budget can't be monopolised by a run of early non-responding hops and
        // starve the ones nearer the destination. Answered hops get whatever
        // budget remains, for their steady keepalive.
        if (max_hop >= 1) {
            for (uint8_t k = 0; k < max_hop; ++k) {
                uint8_t ttl = static_cast<uint8_t>((rr - 1 + k) % max_hop + 1);
                if (!answered(ttl)) try_send(ttl);
            }
            rr = static_cast<uint8_t>(rr % max_hop + 1);
        }
        if (max_hop >= 1) {
            for (uint8_t k = 0; k < max_hop; ++k) {
                uint8_t ttl = static_cast<uint8_t>((rr_ans - 1 + k) % max_hop + 1);
                if (answered(ttl)) try_send(ttl);
            }
            rr_ans = static_cast<uint8_t>(rr_ans % max_hop + 1);
        }

        // Deliberate check for a SHORTENED route (e.g. a VPN/tunnel just came
        // up). Once dest_hop_ is set, compute_max_hop() caps every probe at
        // 1..dest_hop_ and try_send()'s recheck logic only ever re-verifies
        // hops already inside that range — nothing ever sends a fresh,
        // TTL-limited probe strictly BELOW dest_hop_, so a real reply from a
        // now-closer destination has no path to ever be observed (see the
        // TimeExceeded-at-dest_hop_ un-freeze above, which only covers the
        // opposite, route-lengthened direction). Send exactly one legacy
        // probe at ttl = dest_hop_ - 1, targeting the destination directly,
        // on the same kHopRecheckSecs cadence as the ordinary per-hop
        // recheck, tracked separately via dest_shrink_check_at_ so it doesn't
        // disturb that hop's own hop_recheck_at bookkeeping. If the
        // destination genuinely answers (EchoReply, or Unreachable from
        // dest_) at this lower TTL, the ordinary reply-handling arrival logic
        // below shrinks dest_hop_ via its existing min() — this reply's `hop`
        // IS the lower TTL, unlike a stale recheck at the old TTL which could
        // never move it. If instead a real intermediate router answers with
        // TimeExceeded, that's just an ordinary confirmation the path is
        // still that length, handled like any other legacy reply.
        if (dest_hop_ && *dest_hop_ > 1 && now - dest_shrink_check_at_ >= kHopRecheckSecs) {
            uint8_t shrink_ttl = static_cast<uint8_t>(*dest_hop_ - 1);
            if (tokens >= 1.0 && g_pacer().try_take(now)) {
                uint16_t seq = next_seq();
                bool sent = strategy->send_hop_probe(*dest_, shrink_ttl, seq, s.payload_size, paris_checksum_target_);
                if (sent) {
                    pending[seq] = Pending{shrink_ttl, now, false, std::string()};
                    tokens -= 1.0;
                    dest_shrink_check_at_ = now;
                    consecutive_send_fails = 0;
                } else {
                    g_pacer().refund();
                    // Same dead-socket self-heal as the main try_send lambda
                    // above — this send uses the same `sock`, so a failure
                    // here is exactly as meaningful a signal.
                    if (++consecutive_send_fails >= kMaxConsecutiveSendFails) {
                        std::lock_guard<std::mutex> lk(settings_mtx_);
                        rebuild_needed_ = true;
                        consecutive_send_fails = 0;
                    }
                }
            }
        }
        // If probes were due but the pacer held them, wake when the next token
        // is ready rather than busy-spinning.
        if (due_but_paced_out) soonest = (std::min)(soonest, now + 1.0 / kMaxProbeRate);

        // 2) read replies for a short slice (wakes immediately on arrival)
        double slice = (std::min)(soonest, last_emit + 0.25) - now;
        if (slice < 0.005) slice = 0.005;
        // Ceiling above which a reply is treated as a stale queue artifact
        // rather than a real measurement. Floor raised from 2.0s to 5.0s —
        // at a fast probe/timeout config the old floor discarded genuine
        // jitter spikes on lossy/congested links as if they were rate-limit
        // queue artifacts. Still scales with the user's own configured
        // timeout for longer-timeout targets (e.g. an intentionally
        // generous timeout for a known-bad satellite link) rather than a
        // flat cap, since a flat 5s ceiling would silently discard replies
        // the user explicitly configured a >5s timeout to wait for.
        double stale_ceiling = (std::max)(timeout * 2.0, 5.0); // seconds
        // Optional runtime debug tracing: enable by setting NETPULSE_DEBUG=1
        static bool debug_enabled = !!getenv("NETPULSE_DEBUG");
        // Every reply this session will ever see arrives via inbox_, pushed by
        // the shared RX dispatcher thread (Session::dispatch_incoming) after a
        // registry lookup by ICMP id — this thread never reads a socket
        // itself (see PooledSocket/acquire_pooled_socket, transport.hpp).
        // Block on inbox_cv_ up to `slice` seconds, waking immediately the
        // instant the dispatcher delivers something (interrupt-driven, same
        // low-jitter property the old direct select() on our own socket had),
        // then drain whatever's queued.
        std::vector<Incoming> replies;
        {
            std::unique_lock<std::mutex> lk(inbox_mtx_);
            if (inbox_.empty()) {
                inbox_cv_.wait_for(lk, std::chrono::duration<double>(slice), [this] { return !inbox_.empty(); });
            }
            replies.reserve(inbox_.size());
            while (!inbox_.empty()) { replies.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
        }
        for (const auto& inc : replies) {
            if (debug_enabled) {
                fprintf(stderr, "[netpulse] reply seq=%u kind=%d from=%s\n", inc.reply.seq, int(inc.reply.kind), inc.from.c_str());
            }
            // inbox_ only ever holds replies the dispatcher already matched to
            // OUR icmp_id_ (see Session::dispatch_incoming) — this is now just
            // a defensive sanity check, not a live routing path.
            if (inc.reply.id != icmp_id_) continue;
            auto it = pending.find(inc.reply.seq);
            if (it == pending.end()) continue;
            uint8_t hop = it->second.hop;
            double rtt = (inc.at - it->second.sent_at) * 1000.0;
            // Reject replies that come back far LATER than expected. This is
            // the fix for the startup latency "ramp": when an edge briefly
            // rate-limits/queues ICMP, replies can arrive many seconds after
            // they were sent. Matching them as valid makes rtt =
            // (now - sent_at) report that whole queue delay (2s, 5s, …) as if
            // it were real latency — and since probes keep going out every
            // interval, each late reply reports an ever-larger gap, producing
            // the clean linear ramp seen in the graph. A reply this stale is
            // not a valid measurement; its probe is counted as loss in step 3
            // instead, exactly as ping/mtr/PingPlotter treat an over-timeout
            // reply.
            if (rtt > stale_ceiling * 1000.0) { pending.erase(it); continue; }
            if (debug_enabled) {
                fprintf(stderr, "[netpulse] matched pending seq=%u hop=%u rtt=%.1fms\n", inc.reply.seq, hop, rtt);
            }

            if (it->second.direct) {
                // Direct-echo probe: only a genuine Echo Reply from the exact
                // IP we pinged counts as a measurement — this is what makes it
                // immune to Time-Exceeded rate limiting (we're pinging the hop
                // directly, exactly like a standalone `ping <hop-ip>`, not
                // asking it to generate a control-plane ICMP error). Anything
                // else for this seq (stray Unreachable, wrong source) proves
                // nothing either way; leave it pending so it can still get a
                // valid reply, or expire via the timeout sweep below, which
                // counts the miss toward the echo-silent fallback.
                if (inc.reply.kind == ReplyKind::EchoReply && inc.from == it->second.direct_ip) {
                    ensure_hop(hop).push(inc.at, rtt);
                    buffer.push_back(NewPoint{hop, inc.at, rtt});
                    last_reply_at[hop] = inc.at;
                    reply_count[hop]++; // see its own doc comment (declaration, above)
                    last_any_reply_at = inc.at; // session-wide: see the silence-watchdog comment above
                    consecutive_silence_rebuilds = 0; // real reply — see its own doc comment
                    silence_rebuild_count_ = 0; // clears the reconnect-attempts banner too, see its own doc comment
                    if (dest_hop_ && hop == *dest_hop_) consecutive_dest_silence_rebuilds = 0; // see its own doc comment — only THIS hop's reply counts here
                    shared_publish(s.source_addr, predecessor_of(hop), inc.from, inc.at, rtt, id_, hop);
                    echo_ok[hop] = true;
                    echo_miss[hop] = 0;
                    pending.erase(it);
                }
                continue;
            }
            bool from_dest = dest_ && inc.from == *dest_;

            // The destination may have moved FARTHER away since it was first
            // confirmed — e.g. a VPN/WARP tunnel that shortened the visible
            // path got turned off, so the real distance is now longer than
            // dest_hop_. compute_max_hop() caps the probe range at dest_hop_
            // once set, so without this check the window would stay frozen at
            // the stale (VPN-era) hop count forever: the periodic legacy
            // recheck at dest_hop_ (kHopRecheckSecs) would keep landing on an
            // intermediate router instead of the destination, that router's
            // address would silently overwrite the hop while it's still
            // flagged is_dest, and every hop beyond the old dest_hop_ would
            // never be probed again. A genuine Time-Exceeded at exactly
            // dest_hop_ that isn't from the destination is exactly that
            // signal — un-freeze discovery so the window can slide out to the
            // real, longer path.
            if (dest_hop_ && hop == *dest_hop_ && inc.reply.kind == ReplyKind::TimeExceeded) {
                dest_hop_.reset();
                dest_found_at = 0.0;
                max_hop_seen_ = hop; // this position is a confirmed real intermediate hop now
            }

            // ICMP *Unreachable* from something OTHER than the destination is
            // not a transit hop at this TTL — it's a router (very often your own
            // gateway / ISP CGNAT during a link outage) reporting it can't
            // deliver toward the target. Recording its source address here is
            // exactly what paints private/CGNAT IPs (192.168.x, 10.x) across
            // random high hops when the connection drops or restarts: the same
            // box answers probes at many TTLs, so it appears at hop 8, 12, 25…
            // at once, which is impossible for a real transit hop. So we do NOT
            // set it as the hop's address and do NOT let it advance discovery;
            // we count the probe as loss for that hop (we never heard from the
            // real hop at this TTL) and move on. A TimeExceeded, by contrast, IS
            // the genuine hop at this TTL and is recorded normally below.
            if (inc.reply.kind == ReplyKind::Unreachable && !from_dest) {
                if (debug_enabled)
                    fprintf(stderr, "[netpulse] unreachable from %s for hop %u — counted as loss, not a transit hop\n",
                            inc.from.c_str(), hop);
                ensure_hop(hop).push(inc.at, std::nullopt);
                buffer.push_back(NewPoint{hop, inc.at, std::nullopt});
                pending.erase(it);
                continue;
            }

            HopStats& hs = ensure_hop(hop);
            // Detect a route flap (a DIFFERENT device now answers at this TTL)
            // before overwriting the address. Direct-echo probes were only
            // ever aimed at the OLD device's IP, so its echo/recheck state is
            // no longer meaningful for whatever now sits at this position —
            // reset it so the new device gets its own fair discovery+direct-
            // echo-probation cycle instead of inheriting stale state.
            if (hs.address() && *hs.address() != inc.from) {
                tries[hop] = 0;
                next_send[hop] = 0.0;
                echo_ok.erase(hop);
                echo_silent.erase(hop);
                echo_miss.erase(hop);
                legacy_miss.erase(hop);
                legacy_miss_since.erase(hop);
                wipe_count.erase(hop); // a genuinely different device — see wipe_count's doc comment
            }
            hs.set_address(inc.from);
            // A legacy probe just successfully confirmed an address at this
            // hop (whether it's the same device as before, or a flap to a
            // new one) — reset the miss counter driving clear_address()
            // below, same requirement either way: consecutive misses, not
            // total misses, are what should trigger clearing a stale hop.
            legacy_miss[hop] = 0;
            legacy_miss_since.erase(hop);
            hs.push(inc.at, rtt);
            // Not every reply carries an RFC 4884/4950 extension structure —
            // only overwrite the hop's label stack when this one actually had
            // labels, so an ordinary reply right after a labeled one doesn't
            // blank out the last-known stack.
            if (!inc.reply.mpls_labels.empty()) hs.set_mpls_labels(inc.reply.mpls_labels);
            buffer.push_back(NewPoint{hop, inc.at, rtt});
            last_reply_at[hop] = inc.at; // a genuine transit/echo reply landed at this hop
            reply_count[hop]++; // see its own doc comment (declaration, above)
            last_any_reply_at = inc.at; // session-wide: see the silence-watchdog comment above
            consecutive_silence_rebuilds = 0; // real reply — see its own doc comment
            silence_rebuild_count_ = 0; // clears the reconnect-attempts banner too, see its own doc comment
            if (dest_hop_ && hop == *dest_hop_) consecutive_dest_silence_rebuilds = 0; // see its own doc comment — only THIS hop's reply counts here
            hop_recheck_at[hop] = inc.at; // this legacy reply IS a fresh identity re-confirmation

            // Routing-loop detection + two-strikes confirmation + self-heal
            // (see the loop-auditor comment block above kLoopAuditSecs for the
            // full rationale). Runs on every genuine legacy reply — this
            // doubles as continuous re-validation, which is what makes
            // recovery self-driving below, with no separate timer.
            {
                std::optional<uint8_t> dup_at;
                for (const auto& [h, hstat] : hops_) {
                    if (h >= hop) break; // hops_ iterates in increasing key order; only LOWER hops count
                    // An IMMEDIATELY adjacent repeat (h == hop - 1) is not a
                    // routing loop candidate: it's the well-known MPLS/tunnel
                    // artifact where one physical/logical router answers two
                    // consecutive TTLs with the same address because internal
                    // (hidden) tunnel segments don't decrement the visible
                    // TTL — deterministic and permanent, not packets bouncing
                    // between routers. A genuine forwarding loop folds the
                    // path back across a real distance (>= 2 hops), so only
                    // that case is worth the two-strikes confirmation below;
                    // treating this as a loop candidate would falsely and
                    // permanently freeze discovery on every such path (seen
                    // e.g. on Google's IPv6 backbone: hop 4 and hop 5 both
                    // answering as 2404:6800:81e2:340::1).
                    if (h + 1 == hop) continue;
                    if (hstat.address() && *hstat.address() == inc.from) { dup_at = h; break; }
                }
                if (dup_at) {
                    int& n = loop_confirm[std::make_pair(hop, *dup_at)];
                    ++n;
                    if (n >= 2) {
                        // Confirmed on two INDEPENDENT replies — a transient
                        // ECMP-hash coincidence wouldn't repeat, a real loop
                        // does, every time (see build_echo's checksum pinning,
                        // which removes the common IPv4 cause of this at the
                        // root; this counter is the backstop for the rest).
                        if (!loop_at_hop || hop < *loop_at_hop) loop_at_hop = hop;
                        // Dedicated fields (loop_warning_/loop_hop_/
                        // loop_dup_at_), not error_ — a routing loop is an
                        // advisory, every hop's data is still valid, so this
                        // must never make the UI hide the table (see the
                        // Snapshot doc comments in session.hpp). Structured
                        // hop numbers alongside the message so the UI can
                        // highlight exactly the two implicated rows instead
                        // of only showing text.
                        loop_warning_ = "Possible routing loop: hop " + std::to_string(hop) +
                                        " repeats the address seen at hop " + std::to_string(*dup_at);
                        loop_hop_ = hop;
                        loop_dup_at_ = *dup_at;
                    }
                } else if (loop_at_hop && hop >= *loop_at_hop) {
                    // Self-heal: a CLEAN reply at or beyond the current loop
                    // boundary — the frozen hop itself, or the one audit hop
                    // past it, the only positions compute_max_hop() keeps in
                    // scope while a loop is active — means the duplicate is
                    // gone. Deliberately scoped to hop >= *loop_at_hop: an
                    // unrelated EARLIER hop's routine 45s recheck coming back
                    // clean (as it always will, since it was never part of the
                    // duplicate) proves nothing about the actual loop boundary
                    // and must NOT clear it prematurely.
                    loop_at_hop.reset();
                    loop_confirm.clear();
                    loop_warning_.reset();
                    loop_hop_.reset();
                    loop_dup_at_.reset();
                }
            }
            // Publish this real measurement so other targets that traverse the
            // same (predecessor -> this hop IP) edge can adopt it instead of
            // probing that hop themselves (see shared_adopt in try_send). Only
            // genuine replies are published — never a loss — so adoption can
            // only ever copy a real sample.
            shared_publish(s.source_addr, predecessor_of(hop), inc.from, inc.at, rtt, id_, hop);
            if (hop > max_hop_seen_) max_hop_seen_ = hop;
            // Queue this hop's IP for background reverse-DNS the first time we
            // see it (the resolver dedups, so this is cheap to call repeatedly).
            g_rdns().enqueue(inc.from);
            // EchoReply = reached the destination. An Unreachable *from the
            // destination itself* (e.g. ICMP port-unreachable) also means we
            // reached it, so treat both as arrival.
            if (inc.reply.kind == ReplyKind::EchoReply ||
                (inc.reply.kind == ReplyKind::Unreachable && from_dest)) {
                // Treat arrival as reaching the destination TTL. Some
                // networks rewrite source addresses (NATs, load-balancers)
                // so the reply may come from an address different from the
                // original resolved `dest_`. Accept it regardless of
                // `inc.from` so the engine recognises the destination and
                // advances discovery. If the reply's source differs from the
                // resolved address, update the visible `dest_` so the UI
                // reflects the actual responding IP.
                if (!dest_hop_) dest_found_at = inc.at; // first time we reach the destination
                dest_hop_ = dest_hop_ ? (std::min)(*dest_hop_, hop) : hop;
                if (debug_enabled) fprintf(stderr, "[netpulse] set dest_hop=%u dest_ip=%s\n", (unsigned)*dest_hop_, inc.from.c_str());
                if (dest_ && inc.from != *dest_) {
                    dest_ = inc.from; // prefer the actual replying address for display
                }
                // The loop auditor only matters while the real destination is
                // still unknown (compute_max_hop only consults it in that
                // branch) — once genuinely reached, it's moot; clear it so a
                // stale loop advisory doesn't linger.
                if (loop_at_hop) {
                    loop_at_hop.reset();
                    loop_confirm.clear();
                    loop_warning_.reset();
                    loop_hop_.reset();
                    loop_dup_at_.reset();
                }
            }
            pending.erase(it);
        }

        // 3) expire timed-out probes as loss samples (only within the known path)
        now = now_secs();
        // Decay the frontier. max_hop_seen_ must reflect the deepest hop that has
        // answered RECENTLY, not the deepest that ever answered. Without this, a
        // single stray reply during a link outage (e.g. a CGNAT box briefly
        // answering at TTL 28) pins the frontier — and the ghost row it created —
        // in place forever, so after the connection is restored the trace never
        // settles back: the list keeps a sent=0 ghost as its last row and the UI
        // stays stuck on "discovering" instead of resolving to the real state.
        //
        // BUG FIX: recency alone can't tell a single fluke apart from a hop
        // that answered reliably for a long time and just went quiet
        // through an ordinary gap — see reply_count's own doc comment
        // (declaration, above) for the full story and the exact reported
        // symptom this produced. A hop with kEstablishedHopReplies or more
        // confirmed replies THIS session is exempt from decay outright,
        // regardless of how long it's been silent — that's the "this is a
        // real, proven hop, not a fluke" signal the original recency-only
        // check had no way to express.
        const double kFrontierStaleSecs = (std::max)(30.0, interval * 20.0);
        {
            uint8_t fresh_max = 0;
            for (const auto& [h, at] : last_reply_at) {
                bool established = reply_count.count(h) && reply_count.at(h) >= kEstablishedHopReplies;
                if ((established || now - at <= kFrontierStaleSecs) && h > fresh_max) fresh_max = h;
            }
            max_hop_seen_ = fresh_max;
        }
        // Before any hop has answered, do not treat the entire max-hop range as
        // established frontier. That would record the first discovery round as
        // 100% loss for all hops. Only once we have at least one reply do we
        // advance the frontier beyond the first probe horizon.
        uint8_t bound = dest_hop_ ? *dest_hop_
                                  : (max_hop_seen_ ? static_cast<uint8_t>(max_hop_seen_ + 1) : 1);
        for (auto it = pending.begin(); it != pending.end();) {
            if (now - it->second.sent_at >= timeout) {
                uint8_t hop = it->second.hop;
                if (hop <= bound) {
                    ensure_hop(hop).push(now, std::nullopt);
                    buffer.push_back(NewPoint{hop, now, std::nullopt});
                }
                if (it->second.direct && !echo_ok.count(hop)) {
                    // A direct-echo probe timed out and this hop has never
                    // once answered a direct ping — count the miss, and after
                    // kEchoTestTries give up on direct echo for this hop and
                    // fall back to TTL-limited probing (it genuinely doesn't
                    // answer unsolicited pings — e.g. an end host with a
                    // strict ICMP policy — so its rate-limit loss, like
                    // mtr/tracert would show for it, is unavoidable).
                    if (++echo_miss[hop] >= kEchoTestTries) echo_silent[hop] = true;
                } else if (!it->second.direct) {
                    // A LEGACY (TTL-elicited) probe timed out. If this hop
                    // has no address yet, that's just ordinary discovery in
                    // progress — nothing to do. If it DOES have an address,
                    // this is the case set_address()'s flap-detection can
                    // never see: the route changed and the new device is
                    // silently dropping the legacy probe instead of
                    // replying with a different address. Left unhandled,
                    // the old address (still a real, globally-routable IP)
                    // would linger forever and keep answering separate
                    // direct-echo probes sent to it, producing a hop that
                    // blends two different physical routes.
                    //
                    // The wipe below is TIME-BOXED, not just a raw miss
                    // count — see kLegacyMissWindowSecs' comment above for
                    // why. An echo_ok/probationary hop only gets ONE legacy
                    // probe per kHopRecheckSecs, so kLegacyMissThreshold of
                    // those (~kLegacyMissWindowSecs apart) is a real
                    // route-change signal. An echo_silent hop instead gets a
                    // legacy probe EVERY interval (its whole steady-state
                    // channel, no separate direct-echo stream to blend a
                    // stale IP into), so kLegacyMissThreshold consecutive
                    // per-second misses there is just ordinary loss/rate-
                    // limiting on a router that generates Time-Exceeded
                    // slowly — requiring the streak to also span
                    // kLegacyMissWindowSecs of real time is what keeps that
                    // case from wiping the hop (and its whole latency
                    // history) every couple of seconds.
                    auto hit = hops_.find(hop);
                    if (hit != hops_.end() && hit->second.address()) {
                        int& miss = legacy_miss[hop];
                        double& since = legacy_miss_since[hop];
                        if (miss == 0) since = now; // start of a new streak
                        ++miss;
                        // s.legacy_miss_threshold/window_secs are opt-in
                        // per-target overrides (0 = unset) — see their doc
                        // comment in session.hpp. Falls back to the built-in
                        // defaults tuned for a lossy consumer-ISP path.
                        int effective_threshold = s.legacy_miss_threshold > 0
                            ? s.legacy_miss_threshold : kLegacyMissThreshold;
                        double effective_window = s.legacy_miss_window_secs > 0.0
                            ? s.legacy_miss_window_secs : kLegacyMissWindowSecs;
                        if (miss >= effective_threshold && (now - since) >= effective_window) {
                            // Final gate: only wipe a hop that's actually
                            // been LOOKING reliable — see
                            // kGuardedWipeMaxRecentLossPct's comment above.
                            // A chronically lossy hop (already well past
                            // this threshold before the current streak even
                            // started) isn't "falsely healthy" and doesn't
                            // get this protection — it stays put, silence
                            // streak or not, since flapping it every ~90s is
                            // strictly worse than leaving a possibly-stale
                            // address in place a while longer.
                            HopStat recent = hit->second.compute(kGuardedWipeRecentLossWindowSecs);
                            int& wipes = wipe_count[hop];
                            if (recent.loss <= kGuardedWipeMaxRecentLossPct && wipes < kMaxGuardedWipesPerHop) {
                                hit->second.clear_address();
                                ++wipes; // NOT reset below — see wipe_count's doc comment
                                tries[hop] = 0;
                                next_send[hop] = 0.0;
                                echo_ok.erase(hop);
                                echo_silent.erase(hop);
                                echo_miss.erase(hop);
                                legacy_miss.erase(hop);
                                legacy_miss_since.erase(hop);
                            }
                        }
                    }
                }
                it = pending.erase(it);
            } else {
                ++it;
            }
        }

        // 3b) Dead-socket / dead-route self-heal via total silence — see
        // kSilenceRebuildMinSecs's comment above for the full rationale.
        // Gated on max_hop_seen_ (this session has gotten at least one real
        // reply at SOME point) so a target that has NEVER once answered —
        // genuinely unreachable from the start, not a network regression —
        // isn't repeatedly wiped and rediscovered for no benefit; this only
        // fires for a session that WAS getting replies and then went
        // completely silent, which is exactly the sleep/wake/interface-flap
        // signature, and is a much stronger signal than any single hop's own
        // loss (it would take every hop, including the destination, going
        // silent at once for this to be a false positive).
        //
        // silence_backoff_threshold() (session.hpp) extends the base
        // threshold the more consecutive silence-triggered rebuilds have
        // happened without a real reply in between — see its own doc
        // comment for why. The increment is guarded on `!rebuild_needed_`
        // so a still-pending trigger from a moment ago (not yet consumed at
        // the top of the loop) doesn't get double-counted.
        if (max_hop_seen_ > 0) {
            double silence_threshold = silence_backoff_threshold(
                (std::max)(kSilenceRebuildMinSecs, interval * kSilenceRebuildIntervalMult),
                consecutive_silence_rebuilds);
            last_any_reply_at_ = last_any_reply_at; // publish for snapshot() — see its doc comment
            if (now - last_any_reply_at >= silence_threshold) {
                std::lock_guard<std::mutex> lk(settings_mtx_);
                if (!rebuild_needed_) ++consecutive_silence_rebuilds;
                rebuild_needed_ = true;
            }
        }
        // 3c) Destination-specific silence — see kDestSilenceRebuildMinSecs's
        // doc comment above. Independent trigger from 3b: a route where the
        // near hops keep answering forever while the destination itself (or
        // everything past some hop) has gone completely and permanently
        // quiet never satisfies 3b's session-wide check, since SOME hop is
        // always still replying. Either check alone is sufficient to request
        // a rebuild.
        //
        // BUG FIX: this used to share 3b's counter — see
        // consecutive_dest_silence_rebuilds' own doc comment (declaration,
        // above) for why that meant this backoff could never actually
        // accumulate in exactly the case it exists for (a healthy near hop
        // masking a genuinely silent destination). Uses its own dedicated
        // counter now, reset only by the destination hop's own replies.
        if (dest_hop_) {
            auto dra = last_reply_at.find(*dest_hop_);
            double dest_silent_for = dra != last_reply_at.end() ? (now - dra->second) : (now - dest_found_at);
            double dest_silence_threshold = silence_backoff_threshold(
                (std::max)(kDestSilenceRebuildMinSecs, interval * kDestSilenceRebuildIntervalMult),
                consecutive_dest_silence_rebuilds);
            if (dest_silent_for >= dest_silence_threshold) {
                std::lock_guard<std::mutex> lk(settings_mtx_);
                if (!rebuild_needed_) ++consecutive_dest_silence_rebuilds;
                rebuild_needed_ = true;
            }
        }

        // 4) once destination is known, keep contiguous rows 1..dest and flag it
        if (dest_hop_) {
            for (uint8_t h = 1; h <= *dest_hop_; ++h) ensure_hop(h);
            for (auto it = hops_.begin(); it != hops_.end();) {
                if (it->first > *dest_hop_) it = hops_.erase(it);
                else ++it;
            }
        } else {
            // No confirmed destination (discovering / unreachable): drop ghost
            // rows — hops that got an address from a stray reply during an
            // outage/route-flap but are now silent AND sit beyond the current
            // (decayed) frontier. These are exactly the sent=0 phantom rows that
            // otherwise linger past a reconnect and keep the target from
            // resolving to its true state.
            uint8_t keep_bound = max_hop_seen_ ? static_cast<uint8_t>(max_hop_seen_ + 1) : 0;
            for (auto it = hops_.begin(); it != hops_.end();) {
                uint8_t h = it->first;
                auto ra = last_reply_at.find(h);
                bool recent = ra != last_reply_at.end() && (now - ra->second) <= kFrontierStaleSecs;
                bool within_frontier = keep_bound && h <= keep_bound;
                if (!recent && !within_frontier) {
                    last_reply_at.erase(h);
                    it = hops_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& [h, hs] : hops_) hs.set_is_dest(dest_hop_ && h == *dest_hop_);

        // 4b) reverse-DNS names. The blocking getnameinfo runs on the shared
        // resolver thread (g_rdns), never here — this loop only reads the
        // ready cache and enqueues IPs it hasn't named yet, at most ~once a
        // second, so the probe engine is never stalled on a lookup (the reason
        // hostname work was historically kept out of this loop). Resolves LAN
        // hops too, so e.g. the home router shows RT-BE86U-4528.home.arpa.
        if (now - last_rdns >= 1.0) {
            for (auto& [h, hs] : hops_) {
                if (!hs.address() || hs.hostname()) continue;
                if (auto name = g_rdns().lookup(*hs.address())) hs.set_hostname(*name);
                else g_rdns().enqueue(*hs.address());
            }
            last_rdns = now;
        }

        // 5) push a snapshot a few times a second.
        if (now - last_emit >= 0.25) {
            on_update(snapshot(true, std::move(buffer)));
            buffer.clear();
            last_emit = now;
        }
    }
    on_update(snapshot(false, {}));
}

void Session::update_settings(const Settings& s) {
    std::lock_guard<std::mutex> lk(settings_mtx_);
    bool family_or_src =
        (s.family != settings_.family) || (s.source_addr != settings_.source_addr) ||
        (s.privileged != settings_.privileged);
    // BUG FIX: this used to only rebuild on a family/source/privileged
    // change — a dest_port or protocol change (both newly exposed as
    // editable in the UI) got applied directly with NO rebuild at all.
    // Every piece of hop-discovery correlation state (the port_to_ttl map,
    // outstanding in-flight probes, the raw-ICMP registry entries — all
    // keyed on ports derived from the OLD dest_port/protocol scheme) would
    // have kept running unchanged, while new probes started going out under
    // the new port/protocol. The hop table would then blend old, stale
    // per-hop history discovered under the OLD port/protocol with new data
    // from the new one, with no clear boundary between them — the same
    // class of "stale identity" problem HopStats::reset_address already
    // takes care to handle for a route flap, just never extended to cover
    // this. A protocol or destination-port change is exactly as
    // significant as a family/source change: rebuild covers it the same
    // way.
    bool protocol_or_port =
        (s.protocol != settings_.protocol) || (s.dest_port != settings_.dest_port);
    settings_ = s;
    settings_dirty_ = true;
    if (family_or_src || protocol_or_port) rebuild_needed_ = true;
}

Settings Session::settings_snapshot() const {
    std::lock_guard<std::mutex> lk(settings_mtx_);
    return settings_;
}

void Session::force_recheck() {
    force_recheck_needed_.store(true);
}

void Session::request_rebuild() {
    std::lock_guard<std::mutex> lk(settings_mtx_);
    rebuild_needed_ = true;
}

// UDP mode's own control loop — see its declaration's doc comment
// (session.hpp) for why this is a separate function rather than a branch
// inside the main ICMP loop above. Deliberately simpler than that loop:
// no direct-echo optimization (no valid equivalent for UDP — see the doc
// comment), no guarded-wipe/loop-auditor sophistication (a real
// follow-up, not attempted here), just straightforward TTL-limited
// discovery and measurement for the whole session lifetime. What IS
// reused, unmodified: HopStats (ensure_hop/push/compute), the
// SharedHopTable cross-target cache (shared_publish/predecessor_of, same
// Gate-2 edge-key scoping ARCHITECTURE.md §4 describes for ICMP),
// GlobalPacer, and the IcmpOwner registry/RX-dispatcher (registered under
// this session's UDP source port instead of an ICMP id — see
// ProbeUdp::new_flow_pin()'s doc comment for why one fixed port safely
// covers every probe this session ever sends).
void Session::run_udp(std::atomic<bool>* stop, std::atomic<bool>* paused,
                       const std::function<void(const Snapshot&)>& on_update) {
    // Moved to the very top (was previously declared much further down,
    // right before the main loop) specifically so entry/setup logging below
    // can use it too — see the debug lines through setup for why "was
    // run_udp() even reached, and did setup succeed" needed to be
    // answerable independently of whether any send ever fails.
    static bool debug_enabled = !!getenv("NETPULSE_DEBUG");
    if (debug_enabled) fprintf(stderr, "[netpulse-udp] run_udp() starting for target=%s\n", target_.c_str());
    if (debug_enabled) fprintf(stderr, "[netpulse-udp] process elevated=%s (UDP-style tracing is documented by other tools as requiring admin on Windows)\n",
                               process_is_elevated() ? "YES" : "NO");
    resolve();
    if (!dest_ || !family_) {
        if (debug_enabled) fprintf(stderr, "[netpulse-udp] resolve() failed, target=%s — giving up before the loop even starts\n", target_.c_str());
        on_update(snapshot(false, {}));
        return;
    }
    if (debug_enabled) fprintf(stderr, "[netpulse-udp] resolved dest=%s family=%d\n", dest_->c_str(), (int)*family_);
    on_update(snapshot(true, {}));

    Settings s0 = settings_snapshot();
    ProbeUdp probe(*family_, s0.dest_port, s0.source_addr);
    if (!probe.ok()) {
        if (debug_enabled) fprintf(stderr, "[netpulse-udp] ProbeUdp construction failed: %s\n", probe.error().c_str());
        error_ = probe.error();
        on_update(snapshot(false, {}));
        return;
    }
    // A raw ICMP socket, purely for the shared RX dispatcher to receive on —
    // ProbeUdp above only ever sends. Every reply this mode gets (both
    // intermediate-hop Time-Exceeded AND the destination's own Port-
    // Unreachable) arrives via ICMP regardless of the probe's own protocol
    // (see probe_udp.hpp's top comment), so without a live pooled socket in
    // this exact (family, privileged, source) combination, nothing is ever
    // listening and every probe times out even on a perfectly healthy
    // network — previously missing entirely, meaning a UDP-only session
    // (no other ICMP-mode target sharing the same combo already holding one
    // open) could NEVER receive anything. This is not a new privilege
    // requirement particular to this app: classic UDP traceroute has always
    // needed a raw socket to observe the ICMP replies even though the
    // probes themselves are ordinary UDP, so reusing the same
    // s0.privileged gate ICMP mode already uses is correct, not incidental.
    auto icmp_sock = acquire_pooled_socket(*family_, s0.privileged, s0.source_addr);
    if (!icmp_sock->ok()) {
        if (debug_enabled) fprintf(stderr, "[netpulse-udp] acquire_pooled_socket failed: %s\n", icmp_sock->error().c_str());
        error_ = icmp_sock->error();
        on_update(snapshot(false, {}));
        return;
    }
    const uint16_t udp_port = static_cast<uint16_t>(probe.new_flow_pin() & 0xFFFFu);
    if (debug_enabled) fprintf(stderr, "[netpulse-udp] setup complete, udp_port(icmp correlation id)=%u — entering main loop\n", (unsigned)udp_port);
    register_icmp_owner(udp_port, this); // see the registry's own doc comment — key doesn't have to be an actual ICMP id, just process-unique
    struct UnregGuard {
        uint16_t port; Session* self;
        ~UnregGuard() { unregister_icmp_owner(port, self); }
    } unreg{udp_port, this};


    PacerMembership pacer_membership;
    g_rx().ensure_started();

    // Per-TTL scheduling/outstanding-probe tracking — deliberately keyed
    // directly by ttl (not a synthetic sequence number the way the ICMP
    // loop's `pending` map is): UDP mode's actual correlation key,
    // recovered from a reply, already IS a ttl (see
    // ProbeUdp::send_at's doc comment — dest_port_ + ttl round-trips back
    // via ParsedReply::seq), so there's no need for an intermediate
    // sequence-number indirection the way ICMP's id/seq-only matching
    // requires.
    struct Outstanding { double sent_at; };
    std::map<uint8_t, Outstanding> outstanding;
    std::map<uint8_t, double> next_send; // ttl -> earliest time due for its next probe

    auto ensure_hop = [&](uint8_t h) -> HopStats& {
        auto it = hops_.find(h);
        if (it == hops_.end()) it = hops_.emplace(h, HopStats(id_, h)).first;
        return it->second;
    };
    auto predecessor_of = [&](uint8_t hop) -> std::string {
        for (uint8_t h = hop; h-- > 1;) {
            auto it = hops_.find(h);
            if (it != hops_.end() && it->second.address()) return *it->second.address();
        }
        return "SRC";
    };

    double last_snapshot_at = 0;
    double last_heartbeat_at = 0; // debug-only periodic status line — see its own site below

    // See Snapshot::protocol_hint's doc comment (session.hpp). UDP-mode
    // traceroute's completion signal — a Port-Unreachable from the
    // destination — is entirely up to that destination choosing to send
    // one. A great many real destinations (firewalled hosts, most cloud/CDN
    // edges, anything behind a stateful NAT that just drops instead of
    // rejecting) never do, no matter how long or how often this session
    // retries — unlike the ICMP silence watchdog above, there is no local
    // fix (new socket, new port, new identifier) that changes a remote
    // party's decision to stay silent. Left alone, that reads to the user
    // as "stuck discovering" forever with no explanation. Give the
    // destination a fair, generous shot — several full sweeps across the
    // configured TTL range — before saying so; udp_give_up_threshold() (this
    // header, unit-tested in tests/test_core.cpp) is the pure-function form
    // of that decision, deliberately mirroring the ICMP silence watchdog's
    // own order-of-magnitude (kDestSilenceRebuildMinSecs/
    // kDestSilenceRebuildIntervalMult above) rather than inventing an
    // unrelated number.
    const double udp_run_started_at = now_secs();
    // See the send-failure branch below (main loop) for the full
    // rationale — counts sendto() failures across all ttls in a row,
    // regardless of which ttl, to answer "can this session send AT ALL".
    int consecutive_udp_send_fails = 0;

    while (!stop->load()) {
        Settings s = settings_snapshot();
        double now = now_secs();
        std::vector<NewPoint> new_points;

        if (!paused->load()) {
            uint8_t ceiling = dest_hop_ ? *dest_hop_ : s.max_hops;
            for (uint8_t ttl = 1; ttl <= ceiling; ++ttl) {
                if (s.paused_hops.count(ttl)) continue;
                if (outstanding.count(ttl)) continue; // one in flight per hop at a time — simple, avoids the port-reuse ambiguity a second concurrent probe at the same ttl would create
                double due = next_send.count(ttl) ? next_send[ttl] : 0.0;
                if (now < due) continue;
                if (!g_pacer().try_take(now)) break; // global cap — see GlobalPacer's own doc comment, unchanged from ICMP mode
                if (probe.send_hop_probe(*dest_, ttl, 0, s.payload_size, static_cast<uint32_t>(udp_port))) {
                    outstanding[ttl] = Outstanding{now};
                    next_send[ttl] = now + s.probe_interval;
                    if (consecutive_udp_send_fails > 0) {
                        consecutive_udp_send_fails = 0; // a real send got out — whatever was blocking it cleared
                        if (error_) error_.reset();      // see this counter's own doc comment for why only THIS function ever sets error_ inside the loop
                    }
                } else {
                    // BUG FIX: previously left next_send[ttl] untouched on
                    // failure, so a persistently-failing ttl (e.g. sendto()
                    // rejected outright by a firewall/AV) was retried on
                    // EVERY single loop pass — no backoff at all — for
                    // every ttl still below `ceiling` at once. That's both
                    // wasted work (hammering a call that's already known to
                    // fail) and, at high ceiling values, enough attempted
                    // sends per pass to repeatedly exhaust the global pacer
                    // on retries alone, crowding out ttls that might
                    // actually be capable of sending. Give a failed send
                    // the same cadence a successful one gets.
                    next_send[ttl] = now + s.probe_interval;
                    if (debug_enabled) {
                        fprintf(stderr, "[netpulse-udp] send FAILED at ttl=%u errno=%d\n", ttl, probe.last_send_errno());
                    }
                    // Consecutive-failure tracking + a clear, user-visible
                    // explanation — see kMaxConsecutiveSendFails' comment
                    // above for the ICMP-mode sibling of this idea. UDP mode
                    // has no equivalent self-heal to fall back on (no
                    // periodic socket rebuild the way ICMP mode has), so
                    // where ICMP mode can silently retry with a fresh
                    // socket, the only honest thing UDP mode can do once
                    // sendto() itself is persistently failing — not "sent
                    // but no reply", actually failing at the point of
                    // sending — is say so: continuing to spin with an empty
                    // hop table and no explanation is exactly the "stuck
                    // discovering with sent=0 forever" shape this is meant
                    // to catch. Deliberately counted across ALL ttls in a
                    // pass, not per-ttl: the question is "can this session
                    // send AT ALL", not "can it send at this one hop".
                    if (++consecutive_udp_send_fails >= kMaxConsecutiveSendFails) {
                        error_ = "UDP sends are failing (errno " + std::to_string(probe.last_send_errno()) +
                                 ") — this usually means a firewall or antivirus is blocking this app's outbound UDP, "
                                 "or (on Windows) it needs to run as Administrator for raw ICMP. Probing will keep "
                                 "retrying in the background and this message will clear on its own once a send succeeds.";
                    }
                }
            }
        }


        // Drain replies — same wait/drain pattern as the ICMP loop above
        // (inbox_/inbox_cv_, filled by the shared RX dispatcher via
        // push_incoming after a registry lookup by udp_port).
        std::vector<Incoming> replies;
        {
            std::unique_lock<std::mutex> lk(inbox_mtx_);
            if (inbox_.empty()) {
                inbox_cv_.wait_for(lk, std::chrono::milliseconds(200), [this] { return !inbox_.empty(); });
            }
            replies.reserve(inbox_.size());
            while (!inbox_.empty()) { replies.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
        }

        for (const auto& inc : replies) {
            if (debug_enabled) {
                fprintf(stderr, "[netpulse-udp] kind=%d from=%s id=%u seq=%u\n",
                        int(inc.reply.kind), inc.from.c_str(), inc.reply.id, inc.reply.seq);
            }
            if (inc.reply.orig_proto != 17) continue; // not a UDP-embedded original — not ours, ignore (shared socket serves every session)
            if (inc.reply.id != udp_port) continue;   // embedded source port doesn't match ours — not this session's probe
            int recovered_ttl = static_cast<int>(inc.reply.seq) - static_cast<int>(s.dest_port);
            if (recovered_ttl < 1 || recovered_ttl > 255) continue; // malformed/foreign — fail soft, never trust an out-of-range value
            uint8_t ttl = static_cast<uint8_t>(recovered_ttl);
            auto out_it = outstanding.find(ttl);
            if (out_it == outstanding.end()) continue; // no longer waiting on this ttl (already timed out, or a stale duplicate) — ignore
            double rtt = (inc.at - out_it->second.sent_at) * 1000.0;
            outstanding.erase(out_it);

            HopStats& hs = ensure_hop(ttl);
            if (!hs.address() || *hs.address() != inc.from) hs.set_address(inc.from);
            hs.push(inc.at, rtt);
            new_points.push_back(NewPoint{ttl, inc.at, rtt});
            shared_publish(s.source_addr, predecessor_of(ttl), inc.from, inc.at, rtt, id_, ttl);
            if (ttl > max_hop_seen_) max_hop_seen_ = ttl;

            if (inc.reply.kind == ReplyKind::Unreachable) {
                // See UdpEmbeddedReply::dest_port_unreachable's doc comment
                // (probe_udp.hpp) for the same caveat this simplification
                // carries: ANY Unreachable code is treated as "the
                // destination answered" here, not just port-unreachable
                // specifically (icmp.cpp's unified parser doesn't currently
                // preserve the ICMP code, only Time-Exceeded-vs-Unreachable
                // — a reasonable first-version simplification given how
                // deliberately obscure the probed port is, not a precise
                // one; a rare network/host-unreachable from a MIDDLE hop
                // would be misread as "reached the destination" here).
                if (!dest_hop_ || ttl <= *dest_hop_) {
                    dest_hop_ = ttl;
                }
            }
        }
        // Consolidate is_dest to exactly the current dest_hop_ — see the
        // identical fix (and full rationale) in run_tcp() below: setting
        // is_dest directly at the point dest_hop_ changes leaves a stale
        // flag behind on whichever hop was previously (possibly wrongly)
        // marked, if dest_hop_ later moves to a different TTL. This is
        // correct by construction instead — always matches dest_hop_
        // exactly, regardless of update order.
        //
        // Also prunes any hop entry beyond dest_hop_ — same gap the ICMP
        // loop above already closes (see its own "keep contiguous rows
        // 1..dest" comment), just missing here until now. A hop past the
        // confirmed destination can still exist in hops_ (e.g. a TTL whose
        // probe happened to reach the destination too, but wasn't the
        // smallest such TTL — see run_udp()/run_tcp()'s dest_hop_
        // selection, which always prefers the smallest) and would
        // otherwise sit there forever showing real address data frozen at
        // sent=0, since discovery never probes past dest_hop_ again once
        // it's confirmed.
        if (dest_hop_) {
            for (auto& [h, hs] : hops_) hs.set_is_dest(h == *dest_hop_);
            for (auto it = hops_.begin(); it != hops_.end();) {
                if (it->first > *dest_hop_) it = hops_.erase(it);
                else ++it;
            }
            // Destination confirmed (possibly after already having posted
            // the hint below on an earlier pass, e.g. the destination was
            // just slow rather than permanently silent) — the hint no
            // longer describes reality, drop it.
            if (protocol_hint_) protocol_hint_.reset();
        } else {
            // See this function's top-of-loop comment for the full
            // rationale. Only fire once real discovery has actually had a
            // chance to run (max_hop_seen_ > 0 — otherwise hop 1 itself is
            // silent, which is a different, already-surfaced condition:
            // see the frontend's own hop-1-specific firewall hint) and only
            // after several genuine full TTL sweeps' worth of time, so a
            // target that's simply slow to resolve on a long path doesn't
            // get mislabeled early.
            double give_up_threshold = udp_give_up_threshold(s.probe_interval, s.max_hops);
            if (!protocol_hint_ && max_hop_seen_ > 0 && (now - udp_run_started_at) >= give_up_threshold) {
                protocol_hint_ = icmp_error_delivery_hint("UDP");
            }
        }

        // Timeout sweep — anything outstanding past the configured timeout
        // (or a sane default) counts as a lost probe, same convention the
        // ICMP loop uses (push a nullopt rtt).
        double timeout = s.timeout > 0 ? s.timeout : (std::max)(s.probe_interval, 1.0);
        for (auto it = outstanding.begin(); it != outstanding.end();) {
            if (now - it->second.sent_at > timeout) {
                // BUG FIX/FEATURE: this used to push nullopt with no trace
                // anywhere — a request-timeout was silent by design, which is
                // exactly backwards for a probe class where a live report
                // needed to know WHICH port, to WHICH target, was the one that
                // never got an answer. NMAP-style terminology deliberately:
                // "filtered" is the accurate word for "a probe was sent and
                // nothing at all came back within the timeout" — could be a
                // firewall silently dropping it, could be the OS not
                // delivering the reply (see icmp_error_delivery_hint()) — as
                // opposed to "closed" (a definite RST) or "open" (a definite
                // SYN-ACK), which both mean a reply genuinely arrived.
                debug_log("[netpulse-udp] hop=" + std::to_string(int(it->first)) +
                          " filtered (no reply within " + std::to_string(timeout) + "s) target=" + (dest_ ? *dest_ : "?") +
                          " local_port=" + std::to_string(ensure_hop(it->first).local_port()) +
                          " remote_port=" + std::to_string(s.dest_port + it->first) + "\n");
                ensure_hop(it->first).push(now, std::nullopt);
                it = outstanding.erase(it);
            } else {
                ++it;
            }
        }

        if (now - last_snapshot_at >= 0.25 || !new_points.empty()) {
            last_snapshot_at = now;
            on_update(snapshot(true, std::move(new_points)));
        }

        // Debug-only periodic status line — independent of whether any
        // send has ever failed (the earlier debug line) or any reply has
        // ever arrived (the per-reply debug line above): this fires on a
        // flat wall-clock cadence regardless, so "the log has entries but
        // they're all from setup, nothing since" is itself an answer (the
        // main loop is alive and iterating, sends are apparently going out
        // per outstanding.size(), just nothing is coming back) rather than
        // an ambiguous silence indistinguishable from "the loop never
        // iterates at all".
        if (debug_enabled && now - last_heartbeat_at >= 5.0) {
            last_heartbeat_at = now;
            fprintf(stderr, "[netpulse-udp] heartbeat target=%s outstanding=%zu dest_hop=%s max_hop_seen=%u total_hops_tracked=%zu consecutive_send_fails=%d\n",
                    target_.c_str(), outstanding.size(),
                    dest_hop_ ? std::to_string((int)*dest_hop_).c_str() : "(none)",
                    (unsigned)max_hop_seen_, hops_.size(), consecutive_udp_send_fails);
        }
    }
}

// TCP mode's own control loop — see its declaration's doc comment
// (session.hpp) for why this exists separately from both the ICMP loop
// and run_udp(). Two genuinely different completion-detection mechanisms
// run side by side here:
//   1. Hop-discovery (intermediate Time-Exceeded) replies — same
//      ICMP-registry mechanism run_udp() uses, just with per-TTL varying
//      SOURCE ports (probe_tcp.cpp's pin+ttl scheme — see its own doc
//      comment for why TCP can't use one fixed port the way UDP does).
//      Registering multiple ports against the same owner is NOT a
//      registry limitation — it's just a map, nothing stops several keys
//      pointing at the same Session*; the earlier assessment that this
//      needed the registry "generalized" was simply wrong.
//   2. TCP's OWN destination replies (SYN-ACK/RST) — these never touch
//      the ICMP inbox at all. ProbeTcp opens a real (non-raw) socket per
//      probe and detects completion by polling that socket directly
//      (poll_completions()), since a completed connect()/RST is normal
//      socket-layer information, not something arriving on the shared
//      raw ICMP socket.
void Session::run_tcp(std::atomic<bool>* stop, std::atomic<bool>* paused,
                       const std::function<void(const Snapshot&)>& on_update) {
    resolve();
    if (!dest_ || !family_) {
        on_update(snapshot(false, {}));
        return;
    }
    on_update(snapshot(true, {}));

    Settings s0 = settings_snapshot();
    ProbeTcp probe(*family_, s0.dest_port, s0.source_addr);

    // Same reasoning as run_udp(): a raw ICMP socket, purely so the shared
    // RX dispatcher has something to receive hop-discovery replies on —
    // ProbeTcp above only ever originates real (non-raw) TCP sockets for
    // sending, never reads from the ICMP socket itself.
    auto icmp_sock = acquire_pooled_socket(*family_, s0.privileged, s0.source_addr);
    if (!icmp_sock->ok()) {
        error_ = icmp_sock->error();
        on_update(snapshot(false, {}));
        return;
    }

    const uint32_t pin = probe.new_flow_pin();
    // Every hop-discovery port this session has ever registered, so they
    // can all be unregistered on the way out — see UnregGuard below.
    // Populated as each ttl's probe is sent (register_icmp_owner(pin+ttl,
    // this)), not all up front: no reason to reserve ports for hops that
    // may never be probed (paused hops, or hops past the destination).
    std::vector<uint16_t> registered_ports;
    struct UnregGuard {
        std::vector<uint16_t>* ports; Session* self;
        ~UnregGuard() { for (uint16_t p : *ports) unregister_icmp_owner(p, self); }
    } unreg{&registered_ports, this};

    PacerMembership pacer_membership;
    g_rx().ensure_started();

    // Per-TTL scheduling/outstanding-probe tracking for the HOP-DISCOVERY
    // side only — ProbeTcp tracks its own per-probe socket state
    // internally for the completion-polling side; this map is just
    // "which ttls are we currently waiting on an ICMP reply for, and
    // since when" so a real reply can compute an RTT and a stale one can
    // time out into a loss sample.
    struct Outstanding { double sent_at; };
    std::map<uint8_t, Outstanding> outstanding;
    // Confirmed-local-port -> ttl. Replaces the old (reply.id - pin)
    // arithmetic, which silently assumed every bind succeeded — see the
    // registration site below for why that broke hop discovery outright
    // on hosts with reserved port ranges.
    std::map<uint16_t, uint8_t> port_to_ttl;
    std::map<uint8_t, double> next_send;

    auto ensure_hop = [&](uint8_t h) -> HopStats& {
        auto it = hops_.find(h);
        if (it == hops_.end()) it = hops_.emplace(h, HopStats(id_, h)).first;
        return it->second;
    };
    auto predecessor_of = [&](uint8_t hop) -> std::string {
        for (uint8_t h = hop; h-- > 1;) {
            auto it = hops_.find(h);
            if (it != hops_.end() && it->second.address()) return *it->second.address();
        }
        return "SRC";
    };

    double last_snapshot_at = 0;
    double last_direct_send = 0; // per-session, NOT static — see the fix note where it's used below
    static bool debug_enabled = !!getenv("NETPULSE_DEBUG");
    const double tcp_run_started_at = now_secs(); // see the ICMP-error-delivery hint below

    while (!stop->load()) {
        Settings s = settings_snapshot();
        double now = now_secs();
        std::vector<NewPoint> new_points;

        if (!paused->load()) {
            uint8_t ceiling = dest_hop_ ? *dest_hop_ : s.max_hops;
            for (uint8_t ttl = 1; ttl <= ceiling; ++ttl) {
                if (s.paused_hops.count(ttl)) continue;
                if (outstanding.count(ttl)) continue;
                double due = next_send.count(ttl) ? next_send[ttl] : 0.0;
                if (now < due) continue;
                if (!g_pacer().try_take(now)) break;
                uint32_t hop_pin = pin + ttl; // see probe_tcp.cpp's own doc comment — a distinct source port per simultaneous ttl is required, not optional
                if (probe.send_hop_probe(*dest_, ttl, ttl, s.payload_size, hop_pin)) {
                    // BUG FIX: this used to register (hop_pin & 0xFFFF),
                    // i.e. the port we ASKED for. If that bind failed the
                    // packet actually left from a different port, the
                    // router quoted that one, and the reply was discarded
                    // as foreign — so the hop never resolved while the
                    // destination (which replies via poll_completions(),
                    // not ICMP) kept working. Register what the socket is
                    // really bound to; see ProbeTcp::last_bound_port().
                    uint16_t reg_port = probe.last_bound_port();
                    if (reg_port == 0) reg_port = static_cast<uint16_t>(hop_pin & 0xFFFFu); // getsockname() failed outright; fall back rather than not registering at all
                    port_to_ttl[reg_port] = ttl; // reply correlation is a lookup now, not arithmetic
                    ensure_hop(ttl).set_local_port(reg_port); // visible in the hop table / logs -- see HopStat::local_port's doc comment
                    // Register AFTER a successful send, keyed on the exact
                    // port ProbeTcp will have bound — see probe_tcp.cpp's
                    // send_at() for the identical pin+ttl derivation this
                    // must match bit-for-bit.
                    register_icmp_owner(reg_port, this);
                    registered_ports.push_back(reg_port);
                    outstanding[ttl] = Outstanding{now};
                    next_send[ttl] = now + s.probe_interval;
                    // Ports: exactly what a real report needed and did not
                    // have when an unusually slow/inconsistent hop needed
                    // diagnosing — the local port is CONFIRMED (getsockname(),
                    // not assumed), the remote port is this session's fixed
                    // configured target port (TCP/HTTP always connect to the
                    // SAME remote port for every hop; only the local port
                    // varies per ttl, for correlation).
                    debug_log("[netpulse-tcp] sent ttl=" + std::to_string(int(ttl)) +
                              " local_port=" + std::to_string(reg_port) +
                              " remote_port=" + std::to_string(s.dest_port) + "\n");
                }
            }
            // Direct probe — full path to the destination itself, detected
            // exclusively via poll_completions() below, never via the ICMP
            // inbox (see this function's top comment). seq=0 is the
            // sentinel marking "this was the direct probe, not a
            // hop-discovery one" — safe since real hop ttls are always
            // >= 1 (see poll below).
            if (now - last_direct_send >= s.probe_interval && g_pacer().try_take(now)) {
                if (probe.send_direct_probe(*dest_, 0, s.payload_size, pin)) {
                    last_direct_send = now;
                }
            }
        }

        // 1) Hop-discovery replies — same inbox drain pattern as run_udp().
        std::vector<Incoming> replies;
        {
            std::unique_lock<std::mutex> lk(inbox_mtx_);
            if (inbox_.empty()) {
                // BUG FIX: this used to be a flat 100ms wait. In TCP mode
                // that is catastrophic for accuracy, because a TCP
                // SYN-ACK does NOT arrive through this ICMP inbox at all
                // (see step 2 below — direct completions come purely from
                // ProbeTcp's own sockets via poll_completions()). So on any
                // path whose intermediate hops stay quiet — very common,
                // and exactly what a live report showed — the inbox is
                // empty essentially every iteration and this blocked the
                // FULL 100ms before poll_completions() below was allowed to
                // look. Since that call computes rtt as (now - sent_at), a
                // SYN-ACK that really came back in 1ms still got recorded
                // as ~100ms: every target floored at the sleep duration, a
                // LAN router and a distant public resolver both reporting
                // near-identical ~100ms with sub-ms jitter. That is
                // measuring this loop, not the network.
                //
                // With a SYN in flight, wait only briefly so the completion
                // is timed promptly; when genuinely idle, keep the original
                // 100ms so an idle session still costs ~10 wakeups/sec
                // rather than 500.
                auto wait_ms = probe.has_pending() ? std::chrono::milliseconds(2)
                                                   : std::chrono::milliseconds(100);
                inbox_cv_.wait_for(lk, wait_ms, [this] { return !inbox_.empty(); });
            }
            replies.reserve(inbox_.size());
            while (!inbox_.empty()) { replies.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
        }
        for (const auto& inc : replies) {
            if (debug_enabled) {
                fprintf(stderr, "[netpulse-tcp] kind=%d from=%s id=%u seq=%u\n",
                        int(inc.reply.kind), inc.from.c_str(), inc.reply.id, inc.reply.seq);
            }
            if (inc.reply.orig_proto != 6) continue; // not a TCP-embedded original — not ours
            // See port_to_ttl's doc comment — look the ttl up by the port
            // this probe was CONFIRMED to have used, rather than assuming
            // it equals (pin + ttl).
            auto pt_it = port_to_ttl.find(static_cast<uint16_t>(inc.reply.id));
            if (pt_it == port_to_ttl.end()) continue; // not one of ours
            uint8_t ttl = pt_it->second;
            auto out_it = outstanding.find(ttl);
            if (out_it == outstanding.end()) continue; // not currently waiting on this ttl — stale/foreign, ignore
            double rtt = (inc.at - out_it->second.sent_at) * 1000.0;
            outstanding.erase(out_it);

            HopStats& hs = ensure_hop(ttl);
            if (!hs.address() || *hs.address() != inc.from) hs.set_address(inc.from);
            hs.push(inc.at, rtt);
            new_points.push_back(NewPoint{ttl, inc.at, rtt});
            shared_publish(s.source_addr, predecessor_of(ttl), inc.from, inc.at, rtt, id_, ttl);
            if (ttl > max_hop_seen_) max_hop_seen_ = ttl;
        }

        // 2) Direct-probe (and any hop-probe that happened to complete
        // anyway — see Pending::is_direct's doc comment, probe_tcp.hpp)
        // completions — polled from ProbeTcp's own sockets, independent
        // of the ICMP inbox entirely.
        double timeout = s.timeout > 0 ? s.timeout : (std::max)(s.probe_interval * 3.0, 3.0);
        for (const auto& c : probe.poll_completions(now, timeout)) {
            if (c.outcome != ProbeOutcome::DirectReply) continue;
            if (c.seq == 0) {
                // The dedicated direct probe (TTL 255) proves the
                // destination is reachable, but — unlike a hop-discovery
                // probe, which is sent at a real, specific TTL — it has NO
                // way to know the destination's actual topological hop
                // count; TTL 255 is an artificial "definitely enough"
                // value, not a measurement. Guessing a hop number from
                // whatever max_hop_seen_ happens to be at the moment this
                // completes was the actual bug behind hops showing
                // duplicate/inconsistent destination data: a direct
                // connect() frequently completes FASTER than intermediate
                // routers' ICMP Time-Exceeded replies trickle back, so the
                // guess was often made before real discovery had gotten
                // anywhere — and that wrong guess then capped the
                // discovery ceiling below, permanently preventing
                // discovery from ever reaching the real hop count. Only
                // refresh an ALREADY-confirmed destination hop's RTT here;
                // never assign one from this branch.
                if (dest_hop_) {
                    HopStats& hs = ensure_hop(*dest_hop_);
                    if (!hs.address() || *hs.address() != c.from) hs.set_address(c.from);
                    hs.push(c.at, c.rtt_ms);
                    hs.set_tcp_port_open(c.tcp_port_open); // NMAP-style open/closed for the UI — see HopStat::tcp_port_open's doc comment
                    new_points.push_back(NewPoint{*dest_hop_, c.at, c.rtt_ms});
                }
            } else {
                // A hop-discovery probe's own socket completed instead of
                // (or in addition to) getting a Time-Exceeded — the real
                // path is shorter than this ttl. Record it as the
                // destination too, same as above, at ITS ttl rather than
                // the frontier. is_dest consolidation happens once, after
                // the completions loop below — see that comment for why.
                uint8_t ttl = static_cast<uint8_t>(c.seq);
                outstanding.erase(ttl);
                if (!dest_hop_ || ttl <= *dest_hop_) dest_hop_ = ttl;
                HopStats& hs = ensure_hop(ttl);
                if (!hs.address() || *hs.address() != c.from) hs.set_address(c.from);
                hs.push(c.at, c.rtt_ms);
                hs.set_tcp_port_open(c.tcp_port_open);
                new_points.push_back(NewPoint{ttl, c.at, c.rtt_ms});
                if (ttl > max_hop_seen_) max_hop_seen_ = ttl;
            }
        }
        // Consolidate is_dest to exactly the current dest_hop_ — done once
        // here rather than patched into each branch above, since dest_hop_
        // can be revised more than once within the same pass (a
        // hop-discovery completion and the direct probe's own completion
        // can both land in the same poll_completions() batch, in either
        // order) and trying to keep every individual update site correct
        // under all possible orderings is exactly the kind of thing that's
        // easy to get subtly wrong — this is correct by construction
        // instead.
        //
        // Also prunes any hop entry beyond dest_hop_ — matches the ICMP
        // loop's own existing "keep contiguous rows 1..dest" behavior,
        // missing here until now. This is exactly what a lingering
        // "hop 14 with real address data but sent=0" row is: a TTL whose
        // probe once reached the destination too, but wasn't the smallest
        // such TTL, so it never became dest_hop_ — and without this,
        // nothing ever removes it, since discovery stops probing past
        // dest_hop_ once confirmed.
        if (dest_hop_) {
            for (auto& [h, hs] : hops_) hs.set_is_dest(h == *dest_hop_);
            for (auto it = hops_.begin(); it != hops_.end();) {
                if (it->first > *dest_hop_) it = hops_.erase(it);
                else ++it;
            }
        }

        // Timeout sweep for hops still waiting on an ICMP reply that never came.
        for (auto it = outstanding.begin(); it != outstanding.end();) {
            if (now - it->second.sent_at > timeout) {
                // BUG FIX/FEATURE: this used to push nullopt with no trace
                // anywhere — a request-timeout was silent by design, which is
                // exactly backwards for a probe class where a live report
                // needed to know WHICH port, to WHICH target, was the one that
                // never got an answer. NMAP-style terminology deliberately:
                // "filtered" is the accurate word for "a probe was sent and
                // nothing at all came back within the timeout" — could be a
                // firewall silently dropping it, could be the OS not
                // delivering the reply (see icmp_error_delivery_hint()) — as
                // opposed to "closed" (a definite RST) or "open" (a definite
                // SYN-ACK), which both mean a reply genuinely arrived.
                debug_log("[netpulse-tcp] hop=" + std::to_string(int(it->first)) +
                          " filtered (no reply within " + std::to_string(timeout) + "s) target=" + (dest_ ? *dest_ : "?") +
                          " local_port=" + std::to_string(ensure_hop(it->first).local_port()) +
                          " remote_port=" + std::to_string(s.dest_port) + "\n");
                ensure_hop(it->first).push(now, std::nullopt);
                it = outstanding.erase(it);
            } else {
                ++it;
            }
        }

        // See icmp_error_delivery_hint() for the full explanation. Fires only
        // once the destination itself is confirmed reachable (so this is not a
        // dead target) while NO intermediate hop has ever answered after a
        // generous settling period — the exact signature of ICMP errors not
        // being delivered to the raw socket. Cleared if a hop ever does answer.
        if (dest_hop_ && *dest_hop_ > 1) {
            if (max_hop_seen_ > 1) {
                if (protocol_hint_) protocol_hint_.reset();
            } else if (!protocol_hint_ && (now - tcp_run_started_at) >= 25.0) {
                protocol_hint_ = icmp_error_delivery_hint("TCP");
            }
        }

        if (now - last_snapshot_at >= 0.25 || !new_points.empty()) {
            last_snapshot_at = now;
            on_update(snapshot(true, std::move(new_points)));
        }
    }
}

void Session::run_http(std::atomic<bool>* stop, std::atomic<bool>* paused,
                       const std::function<void(const Snapshot&)>& on_update) {
    resolve();
    if (!dest_ || !family_) {
        on_update(snapshot(false, {}));
        return;
    }
    on_update(snapshot(true, {}));

    Settings s0 = settings_snapshot();
    ProbeHttp probe(*family_, s0.dest_port, target_, s0.source_addr);

    // Same reasoning as run_udp(): a raw ICMP socket, purely so the shared
    // RX dispatcher has something to receive hop-discovery replies on —
    // ProbeTcp above only ever originates real (non-raw) TCP sockets for
    // sending, never reads from the ICMP socket itself.
    auto icmp_sock = acquire_pooled_socket(*family_, s0.privileged, s0.source_addr);
    if (!icmp_sock->ok()) {
        error_ = icmp_sock->error();
        on_update(snapshot(false, {}));
        return;
    }

    const uint32_t pin = probe.new_flow_pin();
    // Every hop-discovery port this session has ever registered, so they
    // can all be unregistered on the way out — see UnregGuard below.
    // Populated as each ttl's probe is sent (register_icmp_owner(pin+ttl,
    // this)), not all up front: no reason to reserve ports for hops that
    // may never be probed (paused hops, or hops past the destination).
    std::vector<uint16_t> registered_ports;
    struct UnregGuard {
        std::vector<uint16_t>* ports; Session* self;
        ~UnregGuard() { for (uint16_t p : *ports) unregister_icmp_owner(p, self); }
    } unreg{&registered_ports, this};

    PacerMembership pacer_membership;
    g_rx().ensure_started();

    // Per-TTL scheduling/outstanding-probe tracking for the HOP-DISCOVERY
    // side only — ProbeTcp tracks its own per-probe socket state
    // internally for the completion-polling side; this map is just
    // "which ttls are we currently waiting on an ICMP reply for, and
    // since when" so a real reply can compute an RTT and a stale one can
    // time out into a loss sample.
    struct Outstanding { double sent_at; };
    std::map<uint8_t, Outstanding> outstanding;
    // Confirmed-local-port -> ttl; same rationale as run_tcp()'s copy.
    std::map<uint16_t, uint8_t> port_to_ttl;
    std::map<uint8_t, double> next_send;

    auto ensure_hop = [&](uint8_t h) -> HopStats& {
        auto it = hops_.find(h);
        if (it == hops_.end()) it = hops_.emplace(h, HopStats(id_, h)).first;
        return it->second;
    };
    auto predecessor_of = [&](uint8_t hop) -> std::string {
        for (uint8_t h = hop; h-- > 1;) {
            auto it = hops_.find(h);
            if (it != hops_.end() && it->second.address()) return *it->second.address();
        }
        return "SRC";
    };

    double last_snapshot_at = 0;
    double last_direct_send = 0; // per-session, NOT static — see the fix note where it's used below
    static bool debug_enabled = !!getenv("NETPULSE_DEBUG");

    while (!stop->load()) {
        Settings s = settings_snapshot();
        double now = now_secs();
        std::vector<NewPoint> new_points;

        if (!paused->load()) {
            uint8_t ceiling = dest_hop_ ? *dest_hop_ : s.max_hops;
            for (uint8_t ttl = 1; ttl <= ceiling; ++ttl) {
                if (s.paused_hops.count(ttl)) continue;
                if (outstanding.count(ttl)) continue;
                double due = next_send.count(ttl) ? next_send[ttl] : 0.0;
                if (now < due) continue;
                if (!g_pacer().try_take(now)) break;
                uint32_t hop_pin = pin + ttl; // see probe_tcp.cpp's own doc comment — a distinct source port per simultaneous ttl is required, not optional
                if (probe.send_hop_probe(*dest_, ttl, ttl, s.payload_size, hop_pin)) {
                    // BUG FIX: identical to run_tcp()'s — register the port
                    // the socket is CONFIRMED to have used, not the one we
                    // asked for, or a failed bind silently discards every
                    // intermediate hop's reply. See ProbeHttp::last_bound_port().
                    uint16_t reg_port = probe.last_bound_port();
                    if (reg_port == 0) reg_port = static_cast<uint16_t>(hop_pin & 0xFFFFu); // getsockname() failed outright; fall back rather than not registering at all
                    port_to_ttl[reg_port] = ttl; // lookup, not arithmetic
                    ensure_hop(ttl).set_local_port(reg_port); // visible in the hop table / logs -- see HopStat::local_port's doc comment
                    // Register AFTER a successful send, keyed on the exact
                    // port ProbeTcp will have bound — see probe_tcp.cpp's
                    // send_at() for the identical pin+ttl derivation this
                    // must match bit-for-bit.
                    register_icmp_owner(reg_port, this);
                    registered_ports.push_back(reg_port);
                    outstanding[ttl] = Outstanding{now};
                    next_send[ttl] = now + s.probe_interval;
                    // Ports: exactly what a real report needed and did not
                    // have when an unusually slow/inconsistent hop needed
                    // diagnosing — the local port is CONFIRMED (getsockname(),
                    // not assumed), the remote port is this session's fixed
                    // configured target port (TCP/HTTP always connect to the
                    // SAME remote port for every hop; only the local port
                    // varies per ttl, for correlation).
                    debug_log("[netpulse-http] sent ttl=" + std::to_string(int(ttl)) +
                              " local_port=" + std::to_string(reg_port) +
                              " remote_port=" + std::to_string(s.dest_port) + "\n");
                }
            }
            // Direct probe — full path to the destination itself, detected
            // exclusively via poll_completions() below, never via the ICMP
            // inbox (see this function's top comment). seq=0 is the
            // sentinel marking "this was the direct probe, not a
            // hop-discovery one" — safe since real hop ttls are always
            // >= 1 (see poll below).
            if (now - last_direct_send >= s.probe_interval && g_pacer().try_take(now)) {
                if (probe.send_direct_probe(*dest_, 0, s.payload_size, pin)) {
                    last_direct_send = now;
                }
            }
        }

        // 1) Hop-discovery replies — same inbox drain pattern as run_udp().
        std::vector<Incoming> replies;
        {
            std::unique_lock<std::mutex> lk(inbox_mtx_);
            if (inbox_.empty()) {
                // BUG FIX: identical to run_tcp()'s own wait — see that
                // function's comment for the full explanation. An HTTP
                // probe completes through poll_completions() on its own
                // sockets, NOT through this ICMP inbox, so a flat 100ms
                // wait here floored every measured RTT at ~100ms in exactly
                // the same way.
                auto wait_ms = probe.has_pending() ? std::chrono::milliseconds(2)
                                                   : std::chrono::milliseconds(100);
                inbox_cv_.wait_for(lk, wait_ms, [this] { return !inbox_.empty(); });
            }
            replies.reserve(inbox_.size());
            while (!inbox_.empty()) { replies.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
        }
        for (const auto& inc : replies) {
            if (debug_enabled) {
                fprintf(stderr, "[netpulse-tcp] kind=%d from=%s id=%u seq=%u\n",
                        int(inc.reply.kind), inc.from.c_str(), inc.reply.id, inc.reply.seq);
            }
            if (inc.reply.orig_proto != 6) continue; // not a TCP-embedded original — not ours
            // Look the ttl up by the CONFIRMED port — see port_to_ttl above.
            auto pt_it = port_to_ttl.find(static_cast<uint16_t>(inc.reply.id));
            if (pt_it == port_to_ttl.end()) continue; // not one of ours
            uint8_t ttl = pt_it->second;
            auto out_it = outstanding.find(ttl);
            if (out_it == outstanding.end()) continue; // not currently waiting on this ttl — stale/foreign, ignore
            double rtt = (inc.at - out_it->second.sent_at) * 1000.0;
            outstanding.erase(out_it);

            HopStats& hs = ensure_hop(ttl);
            if (!hs.address() || *hs.address() != inc.from) hs.set_address(inc.from);
            hs.push(inc.at, rtt);
            new_points.push_back(NewPoint{ttl, inc.at, rtt});
            shared_publish(s.source_addr, predecessor_of(ttl), inc.from, inc.at, rtt, id_, ttl);
            if (ttl > max_hop_seen_) max_hop_seen_ = ttl;
        }

        // 2) Direct-probe (and any hop-probe that happened to complete
        // anyway — see Pending::is_direct's doc comment, probe_tcp.hpp)
        // completions — polled from ProbeTcp's own sockets, independent
        // of the ICMP inbox entirely.
        double timeout = s.timeout > 0 ? s.timeout : (std::max)(s.probe_interval * 3.0, 3.0);
        for (const auto& c : probe.poll_completions(now, timeout)) {
            if (c.outcome != ProbeOutcome::DirectReply) continue;
            if (c.seq == 0) {
                // The dedicated direct probe (TTL 255) proves the
                // destination is reachable, but — unlike a hop-discovery
                // probe, which is sent at a real, specific TTL — it has NO
                // way to know the destination's actual topological hop
                // count; TTL 255 is an artificial "definitely enough"
                // value, not a measurement. Guessing a hop number from
                // whatever max_hop_seen_ happens to be at the moment this
                // completes was the actual bug behind hops showing
                // duplicate/inconsistent destination data: a direct
                // connect() frequently completes FASTER than intermediate
                // routers' ICMP Time-Exceeded replies trickle back, so the
                // guess was often made before real discovery had gotten
                // anywhere — and that wrong guess then capped the
                // discovery ceiling below, permanently preventing
                // discovery from ever reaching the real hop count. Only
                // refresh an ALREADY-confirmed destination hop's RTT here;
                // never assign one from this branch.
                if (dest_hop_) {
                    HopStats& hs = ensure_hop(*dest_hop_);
                    if (!hs.address() || *hs.address() != c.from) hs.set_address(c.from);
                    hs.push(c.at, c.rtt_ms);
                    new_points.push_back(NewPoint{*dest_hop_, c.at, c.rtt_ms});
                }
            } else {
                // A hop-discovery probe's own socket completed instead of
                // (or in addition to) getting a Time-Exceeded — the real
                // path is shorter than this ttl. Record it as the
                // destination too, same as above, at ITS ttl rather than
                // the frontier. is_dest consolidation happens once, after
                // the completions loop below — see that comment for why.
                uint8_t ttl = static_cast<uint8_t>(c.seq);
                outstanding.erase(ttl);
                if (!dest_hop_ || ttl <= *dest_hop_) dest_hop_ = ttl;
                HopStats& hs = ensure_hop(ttl);
                if (!hs.address() || *hs.address() != c.from) hs.set_address(c.from);
                hs.push(c.at, c.rtt_ms);
                new_points.push_back(NewPoint{ttl, c.at, c.rtt_ms});
                if (ttl > max_hop_seen_) max_hop_seen_ = ttl;
            }
        }
        // Consolidate is_dest to exactly the current dest_hop_ — done once
        // here rather than patched into each branch above, since dest_hop_
        // can be revised more than once within the same pass (a
        // hop-discovery completion and the direct probe's own completion
        // can both land in the same poll_completions() batch, in either
        // order) and trying to keep every individual update site correct
        // under all possible orderings is exactly the kind of thing that's
        // easy to get subtly wrong — this is correct by construction
        // instead.
        //
        // Also prunes any hop entry beyond dest_hop_ — matches the ICMP
        // loop's own existing "keep contiguous rows 1..dest" behavior,
        // missing here until now. This is exactly what a lingering
        // "hop 14 with real address data but sent=0" row is: a TTL whose
        // probe once reached the destination too, but wasn't the smallest
        // such TTL, so it never became dest_hop_ — and without this,
        // nothing ever removes it, since discovery stops probing past
        // dest_hop_ once confirmed.
        if (dest_hop_) {
            for (auto& [h, hs] : hops_) hs.set_is_dest(h == *dest_hop_);
            for (auto it = hops_.begin(); it != hops_.end();) {
                if (it->first > *dest_hop_) it = hops_.erase(it);
                else ++it;
            }
        }

        // Timeout sweep for hops still waiting on an ICMP reply that never came.
        for (auto it = outstanding.begin(); it != outstanding.end();) {
            if (now - it->second.sent_at > timeout) {
                // BUG FIX/FEATURE: this used to push nullopt with no trace
                // anywhere — a request-timeout was silent by design, which is
                // exactly backwards for a probe class where a live report
                // needed to know WHICH port, to WHICH target, was the one that
                // never got an answer. NMAP-style terminology deliberately:
                // "filtered" is the accurate word for "a probe was sent and
                // nothing at all came back within the timeout" — could be a
                // firewall silently dropping it, could be the OS not
                // delivering the reply (see icmp_error_delivery_hint()) — as
                // opposed to "closed" (a definite RST) or "open" (a definite
                // SYN-ACK), which both mean a reply genuinely arrived.
                debug_log("[netpulse-http] hop=" + std::to_string(int(it->first)) +
                          " filtered (no reply within " + std::to_string(timeout) + "s) target=" + (dest_ ? *dest_ : "?") +
                          " local_port=" + std::to_string(ensure_hop(it->first).local_port()) +
                          " remote_port=" + std::to_string(s.dest_port) + "\n");
                ensure_hop(it->first).push(now, std::nullopt);
                it = outstanding.erase(it);
            } else {
                ++it;
            }
        }

        if (now - last_snapshot_at >= 0.25 || !new_points.empty()) {
            last_snapshot_at = now;
            on_update(snapshot(true, std::move(new_points)));
        }
    }
}

Snapshot Session::snapshot(bool running, std::vector<NewPoint> new_points) const {
    Snapshot s;
    s.target = target_;
    s.dest_ip = dest_;
    s.family = family_;
    s.running = running;
    s.error = error_;
    s.loop_warning = loop_warning_;
    s.loop_hop = loop_hop_;
    s.loop_dup_at = loop_dup_at_;
    s.silence_rebuild_count = silence_rebuild_count_;
    s.last_any_reply_at = last_any_reply_at_;
    s.protocol_hint = protocol_hint_;
    for (const auto& [h, hs] : hops_) {
        s.hops.push_back(hs.compute(settings_.focus_secs));
        if (hs.is_dest()) s.dest_hop = h;
    }
    s.new_points = std::move(new_points);
    return s;
}

} // namespace netpulse
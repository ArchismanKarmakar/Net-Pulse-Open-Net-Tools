#include "netpulse/session.hpp"
#include "netpulse/transport.hpp"
#include "netpulse/platform.hpp"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <cstring>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
#endif

namespace netpulse {

// ---------------------------------------------------------------------------
// Cross-session reply registry (fixes multi-target shared-hop loss).
//
// Each target runs on its own thread with its own raw ICMP socket. On Windows
// especially, inbound ICMP is frequently delivered to just ONE of those sockets
// rather than copied to all of them, so a reply meant for target B lands on
// target A's socket, where it's discarded because the ICMP id isn't A's. The
// owning session then never sees its reply and its shared hops (home router,
// ISP BNG, common intermediates) read as high/100% loss even though the router
// answered — exactly the "0% for one target, 89% for another on the SAME hop"
// symptom.
//
// The registry maps each session's unique ICMP id -> Session*. A session that
// drains a reply whose id isn't its own routes it to the owning session's inbox
// (below), so every reply is processed by exactly the right session regardless
// of which socket the OS happened to hand it to.
namespace {
std::mutex g_reg_mtx;
std::map<uint16_t, Session*> g_registry;
} // namespace

void Session::route_reply_(const Incoming& inc) {
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    auto it = g_registry.find(inc.reply.id);
    if (it != g_registry.end() && it->second && it->second != this) {
        std::lock_guard<std::mutex> ib(it->second->inbox_mtx_);
        it->second->inbox_.push_back(inc);
    }
}

Session::~Session() {
    std::lock_guard<std::mutex> lk(g_reg_mtx);
    auto it = g_registry.find(icmp_id_);
    if (it != g_registry.end() && it->second == this) g_registry.erase(it);
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
// Keyed by (source_addr, responder IP) so targets bound to different egress
// interfaces/VPNs don't share each other's paths. Only REAL replies are ever
// published (never fabricated loss), and adoption only happens in steady state
// (hop address known, not during route discovery) using a sample fresher than
// ~1.5 intervals. Ownership is emergent and self-healing: if the owner's hop
// starts dropping it stops publishing, the entry goes stale within ~1.5
// intervals, adopters fall back to real probing and see the true loss.
namespace {
struct SharedSample {
    double ts = 0;       // epoch seconds of the published reply
    double rtt = 0;      // ms
    const void* owner = nullptr; // publishing Session* (adopt only from a DIFFERENT owner)
};
struct SharedHopTable {
    std::mutex mtx;
    std::unordered_map<std::string, SharedSample> map; // key: source_addr "\x1f" ip
};
inline SharedHopTable& g_shared() { static SharedHopTable* t = new SharedHopTable(); return *t; } // leaked singleton (see g_pacer note)

inline std::string shared_key(const std::string& src, const std::string& ip) {
    std::string k;
    k.reserve(src.size() + 1 + ip.size());
    k += src; k += '\x1f'; k += ip;
    return k;
}
void shared_publish(const std::string& src, const std::string& ip, double ts, double rtt, const void* owner) {
    if (ip.empty()) return;
    SharedHopTable& sh = g_shared();
    std::lock_guard<std::mutex> lk(sh.mtx);
    sh.map[shared_key(src, ip)] = SharedSample{ts, rtt, owner};
}
// If another session has a fresh reply for this hop IP, return its rtt (ms).
std::optional<double> shared_adopt(const std::string& src, const std::string& ip,
                                   const void* self, double now, double max_age) {
    if (ip.empty()) return std::nullopt;
    SharedHopTable& sh = g_shared();
    std::lock_guard<std::mutex> lk(sh.mtx);
    auto it = sh.map.find(shared_key(src, ip));
    if (it == sh.map.end()) return std::nullopt;
    const SharedSample& s = it->second;
    if (s.owner == self) return std::nullopt;          // we're the owner — probe for real
    if (now - s.ts > max_age) return std::nullopt;      // stale — fall back to real probing
    return s.rtt;
}
} // namespace

// ---------------------------------------------------------------------------
// Process-GLOBAL reverse-DNS resolver.
//
// getnameinfo() blocks — often for seconds on a hop with no PTR record or a slow
// resolver — so it must never run on a probe thread (that would stall probe
// scheduling and inflate RTTs). Instead a single background thread services a
// shared work queue and fills a shared cache; every session enqueues the IPs it
// sees and later reads the cache to set hop.hostname. One resolver for the whole
// process also dedups identical IPs across all targets (the router is resolved
// once, not once per target). Unlike the old (never-called) per-session
// resolve_some_hostnames(), this resolves ALL hops including private/LAN ones,
// so a home router with a PTR in its own dnsmasq (e.g. RT-BE86U-4528.home.arpa)
// gets named — exactly what Windows `tracert` shows and NetPulse previously did
// not.
namespace {
struct RdnsResolver {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::string> queue;
    std::unordered_set<std::string> queued;             // in queue or in flight (dedup)
    std::unordered_map<std::string, std::string> cache; // ip -> hostname ("" = looked up, no PTR)
    std::thread th;
    bool started = false;

    void ensure_started() {
        std::lock_guard<std::mutex> lk(mtx);
        if (started) return;
        started = true;
        th = std::thread([this] { worker(); });
        th.detach(); // process-lifetime resolver; no join needed
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
constexpr double kDiscoveryInterval = 0.20; // s — desired retry cadence for UNANSWERED hops while discovering
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
// moment it's found, so it isn't hammered.
constexpr int    kDiscoveryWindow   = 3;    // hops probed beyond the resolved frontier while discovering (small = gentler burst on rate-limiting IPv6 dests)
constexpr int    kFrontierProbes    = 2;    // sends after which an as-yet-unanswered hop still lets the window advance past it

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
constexpr uint8_t kDirectEchoTtl = 64; // full TTL for a direct hop ping (every real path is far shorter)
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

Session::Session(uint64_t id, std::string target, Settings settings)
    : id_(id), target_(std::move(target)), settings_(std::move(settings)) {
    // Per-target-unique ICMP identifier. Every raw ICMP socket in the process
    // receives a copy of every reply, so sessions demultiplex by this id; mixing
    // the monotonically-increasing target id_ in keeps ids distinct across
    // concurrent targets (so one target can't match/consume another's replies)
    // while matching the long-proven v19 behaviour.
    icmp_id_ = static_cast<uint16_t>((static_cast<uint64_t>(now_secs())) ^ id_ ^ 0x4242u);
}

uint16_t Session::next_seq() {
    seq_ = static_cast<uint16_t>(seq_ + 1);
    return seq_;
}

void Session::resolve() {
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
    resolve();
    if (!dest_ || !family_) {
        on_update(snapshot(false, {}));
        return;
    }
    on_update(snapshot(true, {})); // immediate "tracing…" with resolved IP
    { std::lock_guard<std::mutex> lk(g_reg_mtx); g_registry[icmp_id_] = this; } // enable cross-session reply routing
    PacerMembership pacer_membership; // count this target toward the global pacer's scaled rate (RAII: leaves on any return)
    g_rdns().ensure_started();        // spin up the shared reverse-DNS resolver on first target

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
            }
            if (resolved_v4_) { family_ = Family::V4; dest_ = resolved_v4_; }
        }
    }

    auto make_prober = [&]() {
        Settings s = settings_snapshot();
        return std::make_unique<Prober>(*family_, s.privileged, s.source_addr);
    };
    auto prober = make_prober();
    if (!prober->ok()) {
        error_ = prober->error();
        on_update(snapshot(false, {}));
        return;
    }

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
    std::map<uint8_t, double> last_reply_at; // per-hop wall-clock of last REAL reply (drives frontier decay + ghost pruning)
    std::map<uint8_t, bool> echo_ok;         // hop answered a DIRECT ping at least once → measure it by direct echo (rate-limit-free)
    std::map<uint8_t, bool> echo_silent;     // hop answered discovery but never a direct ping → fall back to TTL-limited probing
    std::map<uint8_t, int>  echo_miss;       // consecutive direct-ping misses while still unconfirmed (drives the echo_silent decision)
    std::map<uint8_t, double> hop_recheck_at; // last time this hop got a legacy TTL-elicited re-confirm probe (see kHopRecheckSecs)

    auto ensure_hop = [&](uint8_t h) -> HopStats& {
        auto it = hops_.find(h);
        if (it == hops_.end()) it = hops_.emplace(h, HopStats(h)).first;
        return it->second;
    };

    while (!stop->load()) {
        // Apply any live config change (interface/family rebuild the socket).
        bool rebuild = false;
        {
            std::lock_guard<std::mutex> lk(settings_mtx_);
            if (rebuild_needed_) { rebuild = true; rebuild_needed_ = false; }
            settings_dirty_ = false;
        }
        if (rebuild) {
            // Attempt to re-resolve and recreate the prober. If this transiently
            // fails (DNS or prober), keep the session running (don't return) so
            // the UI spinner and discovery state remain visible; retry shortly.
            resolve(); // family may have changed → re-pick dest/family
            if (!dest_ || !family_) {
                // leave the previous state intact, surface a running snapshot
                on_update(snapshot(true, {}));
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            } else {
                prober = make_prober();
                if (!prober->ok()) {
                    error_ = prober->error();
                    on_update(snapshot(true, {}));
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                } else {
                    // successful rebuild — reset discovery state and resume
                    pending.clear();
                    next_send.clear();
                    dest_hop_.reset();
                    max_hop_seen_ = 0;
                    hops_.clear();
                    dest_found_at = 0.0;       // discovery restarts from scratch
                    tokens = kProbeBurst;
                    last_refill = now_secs();
                    tries.clear();
                    rr = 1;
                    last_reply_at.clear();
                    echo_ok.clear();
                    echo_silent.clear();
                    echo_miss.clear();
                    hop_recheck_at.clear();
                }
            }
        }

        if (paused && paused->load()) {
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
        // While still discovering, use a sliding window (see kDiscoveryWindow):
        // only probe up to kDiscoveryWindow hops past the deepest already-resolved
        // hop, so we walk out to the destination instead of blasting every TTL at
        // it at once (which is what a rate-limiting IPv6 endpoint drops en masse).
        uint8_t max_hop;
        if (dest_hop_) {
            max_hop = *dest_hop_;
        } else {
            uint8_t frontier = 0;
            for (const auto& [h, cnt] : tries) {
                bool resolved = last_reply_at.count(h) > 0 || cnt >= kFrontierProbes;
                if (resolved && h > frontier) frontier = h;
            }
            int window_top = static_cast<int>(frontier) + kDiscoveryWindow;
            int capped = std::min<int>(static_cast<int>(s.max_hops), window_top);
            max_hop = static_cast<uint8_t>(capped < 1 ? 1 : capped);
        }
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
                // independently re-verify what's actually there.
                double& recheck_at = hop_recheck_at[ttl];
                bool due_recheck = have_addr && (now - recheck_at >= kHopRecheckSecs);

                // Shared-hop adoption: if another target on the same egress
                // already has a fresh real reply for this exact hop IP, adopt
                // it as our own sample and DON'T send. This is what stops N
                // targets from each probing the shared router/BNG every
                // interval — the hop sees ~one probe total instead of N.
                bool adopted = false;
                if (have_addr && !due_recheck) {
                    // Freshness bound: a shared sample older than ~1.5 intervals
                    // is ignored, so if the owner stops answering (hop actually
                    // dropping) we fall back to real probing within one interval
                    // and surface the true loss — self-healing, not sticky.
                    auto shared = shared_adopt(s.source_addr, hip, this, now, interval * 1.5 + 0.05);
                    if (shared) {
                        hops_.find(ttl)->second.push(now, *shared);
                        buffer.push_back(NewPoint{ttl, now, *shared});
                        last_reply_at[ttl] = now;
                        nx = now + interval; // steady cadence; we measured via adoption
                        adopted = true;
                    }
                }
                if (adopted) { soonest = (std::min)(soonest, nx); return; }

                // Direct-echo measurement (the fix for shared-hop rate-limit
                // loss, see kDirectEchoTtl above): once a hop's IP is known and
                // it hasn't proven echo-silent, ping that IP directly full-TTL
                // instead of eliciting a Time-Exceeded via *dest_. A due
                // recheck temporarily forces the legacy TTL-limited style so
                // the hop's identity gets independently reconfirmed.
                bool use_direct = have_addr && !echo_silent.count(ttl) && !due_recheck;

                // A real send needs BOTH a per-target token AND a global pacer
                // token, so neither one target nor the whole process can exceed
                // its cap. Take the global token only after the local one is
                // available; refund it if the send itself fails.
                if (tokens >= 1.0 && g_pacer().try_take(now)) {
                    uint16_t seq = next_seq();
                    bool sent = use_direct ? prober->send(hip, kDirectEchoTtl, icmp_id_, seq, s.payload_size)
                                           : prober->send(*dest_, ttl, icmp_id_, seq, s.payload_size);
                    if (!sent) {
                        // If send failed and we're unprivileged and using a
                        // large payload, try a conservative fallback once.
                        size_t fallback = 1432;
                        if (!s.privileged && s.payload_size > fallback) {
                            sent = use_direct ? prober->send(hip, kDirectEchoTtl, icmp_id_, seq, fallback)
                                              : prober->send(*dest_, ttl, icmp_id_, seq, fallback);
                        }
                    }
                    if (sent) {
                        pending[seq] = Pending{ttl, now, use_direct, use_direct ? hip : std::string()};
                        tokens -= 1.0;
                        if (!use_direct) ++tries[ttl];
                        if (due_recheck) recheck_at = now;
                    } else {
                        // do not consume a token or count a try for a failed send;
                        // refund the global token we took, and mark that pacing
                        // prevented sends this round.
                        g_pacer().refund();
                        due_but_paced_out = true;
                    }
                    // Fast cadence only where it helps: an answered hop only
                    // during the post-destination settle window; an unanswered
                    // hop only for its first kDiscoveryTries attempts (after
                    // that it's a non-responder — back off to the steady
                    // interval so it stops consuming the paced budget). Every
                    // other case uses the configured interval.
                    bool fast = have_addr ? settling
                                          : (discovering && tries[ttl] < kDiscoveryTries);
                    nx = now + (fast ? (std::min)(interval, kDiscoveryInterval) : interval);
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
        for (uint8_t ttl = 1; ttl <= max_hop; ++ttl) { if (answered(ttl)) try_send(ttl); if (ttl == 255) break; }
        // If probes were due but the pacer held them, wake when the next token
        // is ready rather than busy-spinning.
        if (due_but_paced_out) soonest = (std::min)(soonest, now + 1.0 / kMaxProbeRate);

        // 2) read replies for a short slice (wakes immediately on arrival)
        double slice = (std::min)(soonest, last_emit + 0.25) - now;
        if (slice < 0.005) slice = 0.005;
        // Ceiling above which a reply is treated as a stale queue artifact
        // rather than a real measurement. Kept a bit above the loss `timeout`
        // (but hard-capped) so a genuinely slow-but-real hop isn't discarded,
        // while the multi-second ramp from ICMP rate-limit queue drainage is.
        double stale_ceiling = (std::max)(timeout * 2.0, 2.0); // seconds
        // Optional runtime debug tracing: enable by setting NETPULSE_DEBUG=1
        static bool debug_enabled = !!getenv("NETPULSE_DEBUG");
        // Gather replies from our own socket AND our inbox — replies that other
        // targets' sockets received on our behalf and routed here (see the
        // cross-session registry note above). Draining both keeps every reply
        // going to the correct session regardless of which socket the OS chose.
        std::vector<Incoming> replies = prober->drain(now + slice);
        {
            std::lock_guard<std::mutex> lk(inbox_mtx_);
            while (!inbox_.empty()) { replies.push_back(std::move(inbox_.front())); inbox_.pop_front(); }
        }
        for (const auto& inc : replies) {
            if (debug_enabled) {
                fprintf(stderr, "[netpulse] reply seq=%u kind=%d from=%s\n", inc.reply.seq, int(inc.reply.kind), inc.from.c_str());
            }
            if (inc.reply.id != icmp_id_) { route_reply_(inc); continue; } // not ours — hand to the owning session
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
                    shared_publish(s.source_addr, inc.from, inc.at, rtt, this);
                    echo_ok[hop] = true;
                    echo_miss[hop] = 0;
                    pending.erase(it);
                }
                continue;
            }
            bool from_dest = dest_ && inc.from == *dest_;

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
            }
            hs.set_address(inc.from);
            hs.push(inc.at, rtt);
            buffer.push_back(NewPoint{hop, inc.at, rtt});
            last_reply_at[hop] = inc.at; // a genuine transit/echo reply landed at this hop
            hop_recheck_at[hop] = inc.at; // this legacy reply IS a fresh identity re-confirmation
            // Publish this real measurement so other targets that traverse the
            // same hop IP can adopt it instead of probing that hop themselves
            // (see shared_adopt in try_send). Only genuine replies are published
            // — never a loss — so adoption can only ever copy a real sample.
            shared_publish(s.source_addr, inc.from, inc.at, rtt, this);
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
        const double kFrontierStaleSecs = (std::max)(30.0, interval * 20.0);
        {
            uint8_t fresh_max = 0;
            for (const auto& [h, at] : last_reply_at)
                if (now - at <= kFrontierStaleSecs && h > fresh_max) fresh_max = h;
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
                }
                it = pending.erase(it);
            } else {
                ++it;
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
    settings_ = s;
    settings_dirty_ = true;
    if (family_or_src) rebuild_needed_ = true;
}

Settings Session::settings_snapshot() const {
    std::lock_guard<std::mutex> lk(settings_mtx_);
    return settings_;
}

Snapshot Session::snapshot(bool running, std::vector<NewPoint> new_points) const {
    Snapshot s;
    s.target = target_;
    s.dest_ip = dest_;
    s.family = family_;
    s.running = running;
    s.error = error_;
    for (const auto& [h, hs] : hops_) {
        s.hops.push_back(hs.compute(settings_.focus_secs));
        if (hs.is_dest()) s.dest_hop = h;
    }
    s.new_points = std::move(new_points);
    return s;
}

} // namespace netpulse
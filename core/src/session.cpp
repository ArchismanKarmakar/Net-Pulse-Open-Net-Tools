#include "netpulse/session.hpp"
#include "netpulse/transport.hpp"
#include "netpulse/platform.hpp"

#include <algorithm>
#include <memory>
#include <chrono>
#include <cstring>
#include <thread>

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

// Delay between successive hops' FIRST probe, so a fresh session ramps up
// gradually instead of bursting every hop's packet in the same instant. 25ms
// matches PingPlotter's own documented fix for this (Options > Engine >
// Packet Send Delay). For 30 hops that's still well under a second of total
// ramp-up (30 * 25ms = 750ms) — imperceptible, but enough to stop looking
// like a burst to a router's rate limiter.
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
//   2. A single GLOBAL token-bucket rate limiter (kMaxProbeRate / kProbeBurst)
//      that paces ALL sends into a smooth, low, constant stream and prioritises
//      the probes that actually advance discovery. This caps the aggregate
//      send rate at or below what the app already produced at steady state with
//      a full hop list (~max_hops/interval), so it never emits a burst larger
//      than normal monitoring — no flood/scan signature, while still finding
//      the route quickly because the limited budget is spent on the hops that
//      haven't answered yet (which, for any TTL >= the destination's distance,
//      reach the destination itself).
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

Session::Session(uint64_t id, std::string target, Settings settings)
    : id_(id), target_(std::move(target)), settings_(std::move(settings)) {
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

    std::optional<std::string> chosen;
    switch (settings_.family) {
        case FamilyPref::V4: chosen = v4; break;
        case FamilyPref::V6: chosen = v6; break;
        case FamilyPref::Auto: chosen = v4 ? v4 : v6; break;
    }
    if (!chosen) {
        error_ = "no address of the requested family for " + target_;
        return;
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
    struct Pending { uint8_t hop; double sent_at; };
    std::map<uint16_t, Pending> pending;
    std::map<uint8_t, double> next_send;
    std::vector<NewPoint> buffer;
    double last_emit = now_secs();
    double dest_found_at = 0.0;         // wall-clock of the FIRST reply from the destination (0 = not yet)
    double tokens = kProbeBurst;        // global send-rate token bucket (see kMaxProbeRate)
    double last_refill = now_secs();
    std::map<uint8_t, int> tries;       // per-hop probe attempts (drives fast->steady backoff of non-responders)
    uint8_t rr = 1;                     // round-robin cursor so the paced budget is shared fairly across unanswered hops

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
        uint8_t max_hop = dest_hop_ ? *dest_hop_ : s.max_hops;
        double now = now_secs();

        // Global token-bucket pacer. Refill toward kMaxProbeRate (capped at a
        // small burst) and charge one token per actual send below. This makes
        // the aggregate ICMP send rate a smooth, constant stream that can NEVER
        // exceed kMaxProbeRate — no matter how many hops are "due" — which is
        // what keeps fast discovery from looking like an ICMP flood/scan to AV.
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
            double& nx = next_send[ttl];
            // First-probe stagger (as before): ramp across hops instead of one
            // simultaneous burst. Subsequent cadence is carried by `nx`.
            if (nx == 0.0) nx = now + (ttl - 1) * kSendStaggerSecs;
            if (now >= nx) {
                if (tokens >= 1.0) {
                    uint16_t seq = next_seq();
                    if (prober->send(*dest_, ttl, icmp_id_, seq, s.payload_size))
                        pending[seq] = Pending{ttl, now};
                    tokens -= 1.0;
                    ++tries[ttl];
                    // Fast cadence only where it helps: an answered hop only
                    // during the post-destination settle window; an unanswered
                    // hop only for its first kDiscoveryTries attempts (after
                    // that it's a non-responder — back off to the steady
                    // interval so it stops consuming the paced budget). Every
                    // other case uses the configured interval.
                    bool fast = answered(ttl) ? settling
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
        for (const auto& inc : prober->drain(now + slice)) {
            if (debug_enabled) {
                fprintf(stderr, "[netpulse] reply seq=%u kind=%d from=%s\n", inc.reply.seq, int(inc.reply.kind), inc.from.c_str());
            }
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
            hs.set_address(inc.from);
            hs.push(inc.at, rtt);
            buffer.push_back(NewPoint{hop, inc.at, rtt});
            if (hop > max_hop_seen_) max_hop_seen_ = hop;
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
        uint8_t bound = dest_hop_ ? *dest_hop_
                                  : (max_hop_seen_ ? static_cast<uint8_t>(max_hop_seen_ + 1) : s.max_hops);
        for (auto it = pending.begin(); it != pending.end();) {
            if (now - it->second.sent_at >= timeout) {
                uint8_t hop = it->second.hop;
                if (hop <= bound) {
                    ensure_hop(hop).push(now, std::nullopt);
                    buffer.push_back(NewPoint{hop, now, std::nullopt});
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
        }
        for (auto& [h, hs] : hops_) hs.set_is_dest(dest_hop_ && h == *dest_hop_);

        // 5) push a snapshot a few times a second.
        // Hostname resolution can block for multiple seconds on reverse DNS
        // lookups, which delays probe scheduling and causes the visible stats
        // stream to stall. Keep the probe engine responsive by omitting
        // hostname work from the tight loop.
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

void Session::resolve_some_hostnames() {
    int done = 0;
    for (auto& [h, hs] : hops_) {
        if (hs.hostname() || !hs.address()) continue;
        const std::string& ip = *hs.address();
        sockaddr_storage ss{};
        socklen_t slen = 0;
        if (ip.find(':') != std::string::npos) {
            auto* s6 = reinterpret_cast<sockaddr_in6*>(&ss);
            s6->sin6_family = AF_INET6;
            inet_pton(AF_INET6, ip.c_str(), &s6->sin6_addr);
            slen = sizeof(sockaddr_in6);
        } else {
            auto* s4 = reinterpret_cast<sockaddr_in*>(&ss);
            s4->sin_family = AF_INET;
            inet_pton(AF_INET, ip.c_str(), &s4->sin_addr);
            slen = sizeof(sockaddr_in);
        }
        char host[NI_MAXHOST] = {0};
        if (getnameinfo(reinterpret_cast<sockaddr*>(&ss), slen, host, sizeof(host), nullptr, 0, 0) == 0) {
            hs.set_hostname(std::string(host));
        } else {
            hs.set_hostname(ip);
        }
        if (++done >= 3) break;
    }
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
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
constexpr double kSendStaggerSecs = 0.025;

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
            resolve(); // family may have changed → re-pick dest/family
            if (!dest_ || !family_) { on_update(snapshot(false, {})); return; }
            prober = make_prober();
            if (!prober->ok()) { error_ = prober->error(); on_update(snapshot(false, {})); return; }
            pending.clear();
            next_send.clear();
            dest_hop_.reset();
            max_hop_seen_ = 0;
            hops_.clear();
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

        // 1) send any probes that are due
        double soonest = now + 0.25;
        for (uint8_t ttl = 1; ttl <= max_hop; ++ttl) {
            double& nx = next_send[ttl];
            // Stagger each hop's FIRST send instead of firing all of them in
            // the same instant. Sending to every hop simultaneously (up to
            // `max_hops` packets within microseconds of each other) is
            // exactly the "parallel burst" pattern that trips ICMP
            // rate-limiting on routers (RFC 1812) — this is the same fix
            // PingPlotter itself exposes as "Packet Send Delay" in
            // Edit > Options > Engine, and mtr's inherently sequential,
            // one-hop-at-a-time design avoids the burst altogether. Once
            // staggered, each hop's own `interval` cadence naturally
            // preserves that offset on every subsequent round, so this only
            // needs to happen once, here.
            if (nx == 0.0) nx = now + (ttl - 1) * kSendStaggerSecs;
            if (now >= nx) {
                uint16_t seq = next_seq();
                if (prober->send(*dest_, ttl, icmp_id_, seq, s.payload_size))
                    pending[seq] = Pending{ttl, now};
                nx = now + interval;
            }
            soonest = (std::min)(soonest, nx);
            if (ttl == 255) break;
        }

        // 2) read replies for a short slice (wakes immediately on arrival)
        double slice = (std::min)(soonest, last_emit + 0.25) - now;
        if (slice < 0.005) slice = 0.005;
        // Ceiling above which a reply is treated as a stale queue artifact
        // rather than a real measurement. Kept a bit above the loss `timeout`
        // (but hard-capped) so a genuinely slow-but-real hop isn't discarded,
        // while the multi-second ramp from ICMP rate-limit queue drainage is.
        double stale_ceiling = (std::max)(timeout * 2.0, 2.0); // seconds
        for (const auto& inc : prober->drain(now + slice)) {
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
            HopStats& hs = ensure_hop(hop);
            hs.set_address(inc.from);
            hs.push(inc.at, rtt);
            buffer.push_back(NewPoint{hop, inc.at, rtt});
            if (hop > max_hop_seen_) max_hop_seen_ = hop;
            if (inc.reply.kind == ReplyKind::EchoReply && inc.from == *dest_)
                dest_hop_ = dest_hop_ ? (std::min)(*dest_hop_, hop) : hop;
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

        // 5) push a snapshot a few times a second
        if (now - last_emit >= 0.25) {
            resolve_some_hostnames();
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

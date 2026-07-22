#include "netpulse/ping_run.hpp"
#include "netpulse/platform.hpp"
#include "netpulse/stats.hpp" // now_secs()

#include <map>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <sys/socket.h>
#endif

namespace netpulse {

PingRun::PingRun(uint64_t id, PingConfig cfg) : id_(id), cfg_(std::move(cfg)) {
    // Same derivation shape as Session's icmp_id_/paris_checksum_target_
    // (session.cpp) — stable for this run's lifetime, and a different XOR
    // salt than Session's so a PingRun and a Session started in the same
    // clock tick with coincidentally equal ids still land on different
    // wire ids. Collision odds are already astronomically low from the
    // time-mixing alone; this is tidiness, not a load-bearing guarantee
    // (Session's own comment makes the same point).
    icmp_id_ = static_cast<uint16_t>((static_cast<uint64_t>(now_secs())) ^ id_ ^ 0x50494e47u); // "PING" ~ish
}

PingRun::~PingRun() {
    unregister_icmp_owner(icmp_id_, this);
}

void PingRun::push_incoming(const Incoming& inc) {
    std::lock_guard<std::mutex> lk(inbox_mtx_);
    inbox_.push_back(inc);
    inbox_cv_.notify_one();
}

bool PingRun::resolve() {
    ensure_winsock_ready();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(cfg_.target.c_str(), nullptr, &hints, &res) != 0 || !res) return false;

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
    switch (cfg_.family) {
        case PingFamilyPref::V4: chosen = v4; break;
        case PingFamilyPref::V6: chosen = v6; break;
        case PingFamilyPref::Auto: chosen = v4 ? v4 : v6; break;
    }
    // Same happy-eyeballs-lite fallback as Session::resolve(): a requested
    // family absent but the other present shouldn't just fail outright.
    if (!chosen) {
        if (cfg_.family == PingFamilyPref::V6 && v4) chosen = v4;
        else if (cfg_.family == PingFamilyPref::V4 && v6) chosen = v6;
        else return false;
    }
    dest_ = chosen;
    family_ = (chosen->find(':') != std::string::npos) ? Family::V6 : Family::V4;
    return true;
}

void PingRun::run(std::atomic<bool>* stop,
                   const std::function<void(const PingLine&)>& on_line,
                   const std::function<void()>& on_done) {
    if (!resolve()) {
        on_line(PingLine{0, false, std::nullopt, "", "Could not resolve " + cfg_.target});
        on_done();
        return;
    }

    auto sock = acquire_pooled_socket(*family_, cfg_.privileged, cfg_.source_addr);
    if (!sock->ok()) {
        on_line(PingLine{0, false, std::nullopt, "", sock->error()});
        on_done();
        return;
    }

    register_icmp_owner(icmp_id_, this);

    double interval = cfg_.interval_secs > 0.01 ? cfg_.interval_secs : 1.0;
    double timeout = cfg_.timeout_secs > 0.0 ? cfg_.timeout_secs : (std::max)(interval, 1.0);

    struct Pending { double sent_at; };
    std::map<uint16_t, Pending> pending;
    uint16_t seq = 0;
    int sent_count = 0;
    double next_send = 0.0;

    while (!stop->load()) {
        double now = now_secs();
        bool more_to_send = cfg_.continuous || sent_count < cfg_.count;
        if (more_to_send && now >= next_send) {
            ++seq;
            bool sent = sock->send(*dest_, cfg_.ttl, icmp_id_, seq, cfg_.payload_size);
            if (sent) {
                pending[seq] = Pending{now};
                ++sent_count;
            } else {
                on_line(PingLine{seq, false, std::nullopt, "", "Send failed"});
            }
            next_send = now + interval;
        }

        // Drain whatever the shared dispatcher has queued for us so far,
        // waiting a short bounded time if nothing's there yet — bounded so
        // due sends/timeouts below are still noticed promptly even during a
        // quiet stretch, same cadence reasoning as Session::run()'s own
        // inbox wait.
        std::deque<Incoming> batch;
        {
            std::unique_lock<std::mutex> lk(inbox_mtx_);
            inbox_cv_.wait_for(lk, std::chrono::milliseconds(50), [&] { return !inbox_.empty(); });
            batch.swap(inbox_);
        }
        for (const auto& inc : batch) {
            auto it = pending.find(inc.reply.seq);
            if (it == pending.end()) continue; // stray/duplicate/already-timed-out — drop
            if (inc.reply.kind == ReplyKind::EchoReply) {
                double rtt_ms = (inc.at - it->second.sent_at) * 1000.0;
                on_line(PingLine{inc.reply.seq, true, rtt_ms, inc.from, ""});
            } else if (inc.reply.kind == ReplyKind::Unreachable) {
                on_line(PingLine{inc.reply.seq, false, std::nullopt, inc.from, "Destination unreachable"});
            } else {
                continue; // a TimeExceeded shouldn't arrive for a full-TTL echo; ignore defensively rather than mis-report
            }
            pending.erase(it);
        }

        now = now_secs();
        for (auto pit = pending.begin(); pit != pending.end();) {
            if (now - pit->second.sent_at >= timeout) {
                on_line(PingLine{pit->first, false, std::nullopt, "", "Request timed out"});
                pit = pending.erase(pit);
            } else {
                ++pit;
            }
        }

        if (!more_to_send && pending.empty()) break; // non-continuous run, everything sent and settled
    }

    unregister_icmp_owner(icmp_id_, this);
    on_done();
}

} // namespace netpulse
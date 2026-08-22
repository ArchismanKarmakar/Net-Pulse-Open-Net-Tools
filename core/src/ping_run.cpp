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

    // Both protocols need this: every reply, UDP-probed or not, arrives via
    // ICMP -- see run_udp()'s own doc comment (session.cpp) for the full
    // reasoning, which applies here just as much as it does to the
    // multi-target engine.
    auto sock = acquire_pooled_socket(*family_, cfg_.privileged, cfg_.source_addr);
    if (!sock->ok()) {
        on_line(PingLine{0, false, std::nullopt, "", sock->error()});
        on_done();
        return;
    }

    // BUG FIX: see ensure_rx_dispatcher_started()'s own doc comment
    // (session.hpp) for the full story — without this, nothing was
    // ever guaranteed to actually read the pooled socket above, for
    // either protocol, and a genuinely standalone PingRun run would
    // silently receive nothing at all.
    ensure_rx_dispatcher_started();

    const bool is_udp = cfg_.protocol == PingProtocol::Udp;
    const bool is_tcp = cfg_.protocol == PingProtocol::Tcp;
    if (is_tcp) {
        // No register_icmp_owner() here on purpose — see tcp_probe_'s doc
        // comment: a TCP ping never consults the ICMP path, which is also
        // exactly why it keeps working on Windows without a capture driver.
        tcp_probe_.emplace(*family_, cfg_.tcp_dest_port, cfg_.source_addr);
    } else if (is_udp) {
        // See ProbeUdp's own top comment for why this can be one persistent
        // socket per run rather than TCP's pool of short-lived ones.
        udp_probe_.emplace(*family_, cfg_.udp_dest_port, cfg_.source_addr);
        if (!udp_probe_->ok()) {
            on_line(PingLine{0, false, std::nullopt, "", udp_probe_->error()});
            on_done();
            return;
        }
        // new_flow_pin() performs and CONFIRMS the bind before returning --
        // see its own doc comment (probe_udp.hpp) for why registering
        // anything else here would risk every reply silently going
        // nowhere, exactly the bug that turned out to be real in the
        // multi-target engine's own UDP mode.
        udp_correlation_port_ = static_cast<uint16_t>(udp_probe_->new_flow_pin() & 0xFFFFu);
        register_icmp_owner(udp_correlation_port_, this);
    } else {
        register_icmp_owner(icmp_id_, this);
    }

    double interval = cfg_.interval_secs > 0.01 ? cfg_.interval_secs : 1.0;
    double timeout = cfg_.timeout_secs > 0.0 ? cfg_.timeout_secs : (std::max)(interval, 1.0);

    // ICMP path: keyed directly by the real sequence number -- ICMP's own
    // echo header carries it verbatim, nothing to recover.
    struct Pending { double sent_at; };
    std::map<uint16_t, Pending> pending;
    // UDP path: send_ping_probe() can only vary the destination port across
    // kPingPortWindow values (see its own doc comment, probe_udp.hpp) -- for
    // a long-running or continuous ping, the real sequence number can run
    // well past that, so this is keyed by the WRAPPED value a reply can
    // actually carry back, with the true sequence number kept alongside it
    // purely for display. A collision (two genuinely-still-outstanding
    // probes wrapping to the same key) is not a realistic concern at any
    // sane interval -- see kPingPortWindow's own sizing rationale.
    struct PendingUdp { double sent_at; uint16_t true_seq; };
    std::map<uint16_t, PendingUdp> pending_udp;

    uint16_t seq = 0;
    int sent_count = 0;
    double next_send = 0.0;

    while (!stop->load()) {
        double now = now_secs();
        bool more_to_send = cfg_.continuous || sent_count < cfg_.count;
        if (more_to_send && now >= next_send) {
            ++seq;
            bool sent;
            if (is_tcp) {
                sent = tcp_probe_->send_direct_probe(*dest_, seq, cfg_.payload_size, 0);
                if (sent) pending[seq] = Pending{now};
                // BUG FIX: ping_run.cpp had ZERO debug_log() calls anywhere,
                // for any protocol -- the main MTR engine (session.cpp) was
                // fully instrumented; this file, backing the whole Ping tab,
                // never was. That's the actual cause of a live report that
                // TCP logging "isn't visible" -- there was nothing to see,
                // for ANY protocol here, not a TCP-specific defect.
                debug_log("[netpulse-ping-tcp] " + std::string(sent ? "sent" : "SEND FAILED") +
                          " seq=" + std::to_string(seq) +
                          " local_port=" + std::to_string(tcp_probe_->last_bound_port()) +
                          " remote_port=" + std::to_string(cfg_.tcp_dest_port) + "\n");
            } else if (is_udp) {
                sent = udp_probe_->send_ping_probe(*dest_, seq, cfg_.payload_size, udp_correlation_port_);
                if (sent) pending_udp[static_cast<uint16_t>(seq % ProbeUdp::kPingPortWindow)] = PendingUdp{now, seq};
                debug_log("[netpulse-ping-udp] " + std::string(sent ? "sent" : "SEND FAILED") +
                          " seq=" + std::to_string(seq) +
                          " local_port=" + std::to_string(udp_correlation_port_) +
                          " remote_port=" + std::to_string(static_cast<uint16_t>(cfg_.udp_dest_port + (seq % ProbeUdp::kPingPortWindow))) + "\n");
            } else {
                sent = sock->send(*dest_, cfg_.ttl, icmp_id_, seq, cfg_.payload_size);
                if (sent) pending[seq] = Pending{now};
                debug_log("[netpulse-ping-icmp] " + std::string(sent ? "sent" : "SEND FAILED") +
                          " seq=" + std::to_string(seq) + " icmp_id=" + std::to_string(icmp_id_) + "\n");
            }
            if (sent) {
                ++sent_count;
            } else {
                on_line(PingLine{seq, false, std::nullopt, "", "Send failed"});
            }
            next_send = now + interval;
        }

        // Drain whatever the shared dispatcher has queued for us so far,
        // waiting a short bounded time if nothing's there yet -- bounded so
        // due sends/timeouts below are still noticed promptly even during a
        // quiet stretch, same cadence reasoning as Session::run()'s own
        // inbox wait.
        // TCP completions come from the probe's own sockets, not the inbox.
        if (is_tcp) {
            for (const auto& r : tcp_probe_->poll_completions(now_secs(), timeout)) {
                auto it = pending.find(r.seq);
                if (it == pending.end()) continue;
                if (r.outcome == ProbeOutcome::DirectReply) {
                    // BUG FIX: note was always empty here, so the UI
                    // fell through to the same generic "Reply from" text
                    // ICMP uses -- a TCP reply looked indistinguishable
                    // from an ICMP one even though the underlying evidence
                    // is completely different (a completed handshake vs a
                    // refused connection, neither of which is an ICMP echo
                    // at all). ProbeTcp::poll_completions() already computes
                    // exactly which one happened; this was being silently
                    // discarded before reaching here.
                    const std::string tnote = (r.tcp_port_open
                        ? std::string("Connected (port open)")
                        : std::string("Port closed, but host responded (RST) -- host is reachable"))
                        + " -- local:" + std::to_string(tcp_probe_->last_bound_port())
                        + " remote:" + std::to_string(cfg_.tcp_dest_port);
                    on_line(PingLine{r.seq, true, r.rtt_ms, r.from, tnote});
                    debug_log("[netpulse-ping-tcp] reply seq=" + std::to_string(r.seq) +
                              " kind=" + (r.tcp_port_open ? "SYN-ACK(open)" : "RST(closed)") +
                              " from=" + r.from + " rtt_ms=" + std::to_string(r.rtt_ms) + "\n");
                } else {
                    on_line(PingLine{r.seq, false, std::nullopt, r.from, "No response"});
                    debug_log("[netpulse-ping-tcp] seq=" + std::to_string(r.seq) + " no response (filtered/dropped)\n");
                }
                pending.erase(it);
            }
        }

        // BUG FIX: this used to be a flat 50ms wait, unconditionally,
        // every iteration -- the EXACT same bug already found and fixed in
        // run_tcp() (session.cpp) many rounds earlier in this project, just
        // never propagated to this second place it independently existed.
        // TCP completions arrive via tcp_probe_->poll_completions() above,
        // NEVER through this ICMP inbox, so with a TCP probe outstanding this
        // wait always ran to its full length before the completion was ever
        // checked -- flooring every TCP ping RTT at ~50ms regardless of the
        // real value. Live evidence: 8.8.8.8 and 1.1.1.1 (genuinely different
        // real RTTs -- 30ms and 27ms respectively, confirmed by the SAME
        // machine's own TCP traceroute to the same destinations) both showed
        // ~50ms in TCP ping specifically, which is possible only if both were
        // being floored at the same constant, not actually measured.
        std::deque<Incoming> batch;
        {
            std::unique_lock<std::mutex> lk(inbox_mtx_);
            auto wait_ms = (is_tcp && tcp_probe_->has_pending()) ? std::chrono::milliseconds(2)
                                                                : std::chrono::milliseconds(50);
            inbox_cv_.wait_for(lk, wait_ms, [&] { return !inbox_.empty(); });
            batch.swap(inbox_);
        }
        for (const auto& inc : batch) {
            if (is_udp) {
                // See ParsedReply::orig_proto's doc comment (icmp.hpp): a
                // UDP-embedded reply's `seq` is the embedded packet's
                // DESTINATION port, not a literal sequence number -- recover
                // the wrapped key send_ping_probe() actually used.
                if (inc.reply.orig_proto != 17) continue; // not a UDP-embedded reply -- stray, ignore
                uint16_t wrapped = static_cast<uint16_t>(inc.reply.seq - cfg_.udp_dest_port);
                auto it = pending_udp.find(wrapped);
                if (it == pending_udp.end()) continue; // stray/duplicate/already-timed-out -- drop
                if (inc.reply.kind == ReplyKind::Unreachable) {
                    // The closest UDP analog to a successful ICMP echo:
                    // nothing is listening on that port, but the destination
                    // itself is what said so -- proof it's alive and
                    // reachable, which is the actual thing a ping measures.
                    double rtt_ms = (inc.at - it->second.sent_at) * 1000.0;
                    // inc.reply.seq IS the actual remote port this specific
                    // probe used (send_ping_probe() varies it per seq, for
                    // correlation -- see that method's own doc comment,
                    // probe_udp.hpp); udp_correlation_port_ is the single
                    // confirmed local port the whole session shares.
                    std::string unote = "Port unreachable (destination is reachable) -- local:" +
                        std::to_string(udp_correlation_port_) + " remote:" + std::to_string(inc.reply.seq);
                    on_line(PingLine{it->second.true_seq, true, rtt_ms, inc.from, unote});
                    debug_log("[netpulse-ping-udp] reply seq=" + std::to_string(it->second.true_seq) +
                              " kind=Port-Unreachable from=" + inc.from + " rtt_ms=" + std::to_string(rtt_ms) + "\n");
                } else {
                    continue; // TimeExceeded shouldn't arrive for a full-TTL (255) probe; ignore defensively rather than mis-report
                }
                pending_udp.erase(it);
            } else {
                auto it = pending.find(inc.reply.seq);
                if (it == pending.end()) continue; // stray/duplicate/already-timed-out -- drop
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
        }

        now = now_secs();
        if (is_udp) {
            for (auto pit = pending_udp.begin(); pit != pending_udp.end();) {
                if (now - pit->second.sent_at >= timeout) {
                    // BUG FIX: this used to carry zero detail beyond "timed
                    // out" -- exactly the piece missing from a real log
                    // where every SUCCESSFUL reply showed local/remote
                    // ports but every failure showed nothing at all,
                    // making a timeout much harder to correlate with a
                    // specific probe than a success was. pit->first IS the
                    // wrapped port offset send_ping_probe() actually used
                    // (see PendingUdp's own doc comment above) -- the real
                    // remote port is just cfg_.udp_dest_port + that.
                    std::string tonote = "Request timed out -- local:" + std::to_string(udp_correlation_port_) +
                        " remote:" + std::to_string(static_cast<uint16_t>(cfg_.udp_dest_port + pit->first));
                    on_line(PingLine{pit->second.true_seq, false, std::nullopt, "", tonote});
                    pit = pending_udp.erase(pit);
                } else {
                    ++pit;
                }
            }
        } else {
            for (auto pit = pending.begin(); pit != pending.end();) {
                if (now - pit->second.sent_at >= timeout) {
                    // TCP: local port varies per probe (a fresh socket each
                    // time), so it can only be read from the probe object
                    // right now, not reconstructed after the fact the way
                    // UDP's can -- best-effort: current confirmed port,
                    // which for a request that never got a reply is very
                    // likely still this same one (nothing else has run
                    // since). ICMP has no ports at all; note stays plain.
                    std::string tonote = is_tcp
                        ? ("Request timed out -- local:" + std::to_string(tcp_probe_->last_bound_port()) +
                           " remote:" + std::to_string(cfg_.tcp_dest_port))
                        : std::string("Request timed out");
                    on_line(PingLine{pit->first, false, std::nullopt, "", tonote});
                    pit = pending.erase(pit);
                } else {
                    ++pit;
                }
            }
        }

        bool nothing_pending = is_udp ? pending_udp.empty() : pending.empty();
        if (!more_to_send && nothing_pending) break; // non-continuous run, everything sent and settled
    }

    if (is_tcp) { /* nothing registered — see setup */ }
    else if (is_udp) unregister_icmp_owner(udp_correlation_port_, this);
    else unregister_icmp_owner(icmp_id_, this);
    on_done();
}

} // namespace netpulse

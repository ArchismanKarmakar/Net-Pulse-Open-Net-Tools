#include "netpulse/probe_http.hpp"
#include "netpulse/platform.hpp"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socklen_t = int;
#  define CLOSESOCK closesocket
#  define LASTERR WSAGetLastError()
#  define WOULDBLOCK WSAEWOULDBLOCK
#  define INPROGRESS WSAEWOULDBLOCK
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <cerrno>
#  define CLOSESOCK ::close
#  define LASTERR errno
#  define WOULDBLOCK EWOULDBLOCK
#  define INPROGRESS EINPROGRESS
#endif

// Plain HTTP only — deliberately no HTTPS/TLS here. The overwhelming
// majority of real HTTP deployments today are 443/TLS, which is exactly
// why this is limited: TLS needs a real library integration (OpenSSL/
// Schannel/BoringSSL), a genuinely separate, substantial undertaking, not
// a small extension of a plain TCP+text-request flow like this one. This
// class is deliberately scoped to what's honestly achievable and
// verifiable in one pass — a real, working plain-HTTP check — rather than
// a half-built TLS attempt.

namespace netpulse {

static inline double now_secs_local() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Monotonic companion to now_secs_local(). Never steps backward and is
// unaffected by NTP corrections or sleep/wake wall-clock jumps — see
// Pending::sent_at_mono's doc comment for why durations must use this
// while timestamps keep using the wall clock.
static inline double now_mono_local() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}


ProbeHttp::ProbeHttp(Family family, uint16_t dest_port, std::string host, std::string source)
    : family_(family), dest_port_(dest_port), host_(std::move(host)), source_(std::move(source)) {
    ensure_winsock_ready();
    hop_prober_ = std::make_unique<ProbeTcp>(family_, dest_port_, source_);
}

ProbeHttp::~ProbeHttp() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [fd, p] : pending_) CLOSESOCK(static_cast<int>(fd));
}

uint32_t ProbeHttp::new_flow_pin() {
    return hop_prober_->new_flow_pin();
}

bool ProbeHttp::send_hop_probe(const std::string& dest_ip, uint8_t ttl, uint16_t seq,
                                size_t payload, uint32_t pin) {
    return hop_prober_->send_hop_probe(dest_ip, ttl, seq, payload, pin);
}

bool ProbeHttp::send_direct_probe(const std::string& dest_ip, uint16_t seq,
                                   size_t /*payload*/, uint32_t pin) {
    int af = (family_ == Family::V4) ? AF_INET : AF_INET6;
    long long fd = static_cast<long long>(::socket(af, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0) return false;

#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode);
#else
    int flags = fcntl(static_cast<int>(fd), F_GETFL, 0);
    fcntl(static_cast<int>(fd), F_SETFL, flags | O_NONBLOCK);
#endif

    int reuse = 1;
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (pin != 0) {
        uint16_t local_port = static_cast<uint16_t>(pin & 0xFFFFu);
        if (family_ == Family::V4) {
            sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(local_port);
            if (!source_.empty()) ::inet_pton(AF_INET, source_.c_str(), &local.sin_addr);
            ::bind(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&local), sizeof(local));
        } else {
            sockaddr_in6 local{}; local.sin6_family = AF_INET6; local.sin6_port = htons(local_port);
            if (!source_.empty()) ::inet_pton(AF_INET6, source_.c_str(), &local.sin6_addr);
            ::bind(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&local), sizeof(local));
        }
        // BUG FIX: the bind result was discarded here exactly as in
        // probe_tcp.cpp. "Non-fatal" holds for ECMP pinning but NOT for
        // reply correlation — session.cpp registers the port it ASKED for,
        // so a silently-failed bind means every intermediate hop's ICMP
        // Time-Exceeded quotes a different port and is discarded as
        // foreign: the destination keeps working while no hop ever
        // resolves. The real port is read back after connect() below.
    }

    sockaddr_storage dst{};
    socklen_t dst_len = 0;
    if (family_ == Family::V4) {
        auto* d4 = reinterpret_cast<sockaddr_in*>(&dst);
        d4->sin_family = AF_INET; d4->sin_port = htons(dest_port_);
        if (::inet_pton(AF_INET, dest_ip.c_str(), &d4->sin_addr) != 1) { CLOSESOCK(static_cast<int>(fd)); return false; }
        dst_len = sizeof(*d4);
    } else {
        auto* d6 = reinterpret_cast<sockaddr_in6*>(&dst);
        d6->sin6_family = AF_INET6; d6->sin6_port = htons(dest_port_);
        if (::inet_pton(AF_INET6, dest_ip.c_str(), &d6->sin6_addr) != 1) { CLOSESOCK(static_cast<int>(fd)); return false; }
        dst_len = sizeof(*d6);
    }

    int rc = ::connect(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&dst), dst_len);
    if (rc < 0) {
        int err = LASTERR;
        if (err != INPROGRESS) { CLOSESOCK(static_cast<int>(fd)); return false; }
        // Refused (RST) is NOT treated as a valid HTTP-level reply here,
        // unlike plain TCP mode — a closed port proves reachability at
        // the transport layer, but this class specifically claims "an
        // HTTP server answered", which a refused connection does not
        // demonstrate. poll_completions() below only ever completes this
        // on real response bytes.
    }

    // Must be read AFTER connect(): if the bind above failed the socket is
    // still unbound there and getsockname() reports 0 — the OS only assigns
    // the fallback port during connect(). (Verified the hard way in
    // probe_tcp.cpp, where reading it too early returned 0 in exactly the
    // failed-bind case this exists to handle.)
    {
        sockaddr_storage sa{}; socklen_t sl = sizeof(sa);
        uint16_t actual_port = 0;
        if (::getsockname(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&sa), &sl) == 0) {
            actual_port = (sa.ss_family == AF_INET)
                ? ntohs(reinterpret_cast<sockaddr_in*>(&sa)->sin_port)
                : ntohs(reinterpret_cast<sockaddr_in6*>(&sa)->sin6_port);
        }
        last_bound_port_.store(actual_port);
    }
    std::lock_guard<std::mutex> lk(mtx_);
    pending_[fd] = Pending{fd, dest_ip, seq, now_secs_local(), now_mono_local(), false, false};
    return true;
}

bool ProbeHttp::has_pending() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !pending_.empty();
}

std::vector<ProbeReply> ProbeHttp::poll_completions(double caller_now, double timeout_secs) {
    // BUG FIX: this used to use the caller's `now` directly for the RTT
    // computation below. session.cpp computes that value at the TOP of its
    // loop iteration — before the inbox wait and before the probe is even
    // sent — while open_probe() stamps sent_at with a fresh clock read at
    // the actual moment of sending, which is strictly LATER. (now - sent_at)
    // was therefore routinely NEGATIVE: a live loopback test measured
    // -0.104ms, which is obviously not a round trip. ProbeTcp deliberately
    // ignores the same parameter and reads its own clock for exactly this
    // reason; do the same here. The parameter is kept (rather than removed)
    // only because it is part of the shared ProbeStrategy signature.
    (void)caller_now;
    const double now = now_secs_local();
    const double mono_now = now_mono_local(); // durations only — see Pending::sent_at_mono
    // Hop-discovery probes that themselves complete a bare TCP connect
    // (the real path is shorter than that TTL) are handled entirely by
    // the internal ProbeTcp — surface those too, same as TCP mode treats
    // this situation: valid transport-level reachability evidence at that
    // TTL, even without a full HTTP response (which only the dedicated
    // direct probe below actually attempts).
    std::vector<ProbeReply> out = hop_prober_->poll_completions(now, timeout_secs);

    std::lock_guard<std::mutex> lk(mtx_);

    for (auto it = pending_.begin(); it != pending_.end();) {
        long long fd = it->first;
        Pending& p = it->second;
        bool drop = false;

        if (!p.connected) {
            fd_set wfds; FD_ZERO(&wfds); FD_SET(static_cast<int>(fd), &wfds);
            fd_set efds; FD_ZERO(&efds); FD_SET(static_cast<int>(fd), &efds);
            timeval tv{0, 0};
            int r = ::select(static_cast<int>(fd) + 1, nullptr, &wfds, &efds, &tv);
            if (r > 0 && (FD_ISSET(static_cast<int>(fd), &wfds) || FD_ISSET(static_cast<int>(fd), &efds))) {
                int so_err = 0; socklen_t so_len = sizeof(so_err);
                ::getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &so_len);
                if (so_err == 0) {
                    p.connected = true; // fall through below to send the request in the same pass
                } else {
                    drop = true; // refused/unreachable — not an HTTP-level answer, see send_direct_probe's doc comment
                }
            }
        }

        if (!drop && p.connected && !p.request_sent) {
            // A minimal, deliberately conservative request: HEAD avoids
            // pulling a full response body over the wire just to confirm
            // the server is there and speaking HTTP (all this needs is
            // ANY response bytes back — see the read side below).
            // Connection: close means the server doesn't need to keep the
            // socket open waiting for a next request that will never
            // come, so it responds and closes promptly on its own end too.
            std::string req = "HEAD / HTTP/1.1\r\nHost: " + host_ + "\r\nUser-Agent: NetPulse\r\nConnection: close\r\n\r\n";
            int wr = ::send(static_cast<int>(fd), req.c_str(), static_cast<int>(req.size()), 0);
            // A request this small always completes in one send() call in
            // practice (well under the OS send buffer / a single TCP
            // segment) — not re-attempting a partial write is a
            // deliberate, documented simplification, not an oversight.
            if (wr > 0) {
                p.request_sent = true;
            } else {
                int err = LASTERR;
                if (err != WOULDBLOCK) drop = true;
                // WOULDBLOCK: send buffer momentarily full — try again next poll, not fatal.
            }
        }

        if (!drop && p.request_sent) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(static_cast<int>(fd), &rfds);
            timeval tv{0, 0};
            int r = ::select(static_cast<int>(fd) + 1, &rfds, nullptr, nullptr, &tv);
            if (r > 0 && FD_ISSET(static_cast<int>(fd), &rfds)) {
                char buf[64];
                int n = ::recv(static_cast<int>(fd), buf, sizeof(buf), 0);
                if (n > 0) {
                    // Got real response bytes — an HTTP server answered.
                    // RTT is measured end-to-end (connect + request +
                    // first response byte), the genuinely meaningful
                    // number for "is this HTTP server responsive", not
                    // just "is this port open".
                    out.push_back(ProbeReply{ProbeOutcome::DirectReply, p.dest_ip, now, (mono_now - p.sent_at_mono) * 1000.0, p.seq, {}});
                    drop = true;
                } else if (n == 0) {
                    drop = true; // graceful close with no bytes at all — not a valid HTTP response
                } else {
                    int err = LASTERR;
                    if (err != WOULDBLOCK) drop = true; // a real read error — give up on this one
                }
            }
        }

        if (!drop && mono_now - p.sent_at_mono > timeout_secs) drop = true;

        if (drop) {
            CLOSESOCK(static_cast<int>(fd));
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

} // namespace netpulse

#include "netpulse/probe_tcp.hpp"
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
#  define ISCONNREFUSED(e) ((e) == WSAECONNREFUSED)
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
#  define ISCONNREFUSED(e) ((e) == ECONNREFUSED)
#endif

namespace netpulse {

static inline uint16_t be16(const uint8_t* b) { return static_cast<uint16_t>((b[0] << 8) | b[1]); }
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


// -------------------------------------------------- embedded-TCP parsing

// Mirrors icmp.cpp's parse_v4/parse_v6 structure and bounds-checking style
// closely — same fail-soft posture (any malformed/truncated buffer yields
// nullopt, never UB), but reading a TCP header's layout instead of an
// ICMP one at the embedded-original-packet offset. A TCP header's first 4
// bytes are source port (2) + dest port (2), not an id/seq pair — this
// packs source port into ParsedReply::id and the low 16 bits of the
// sequence number (bytes 4-7) into ParsedReply::seq, giving the SAME
// two-field shape ICMP's id/seq matching already uses, just filled from
// different bits. Session-side matching code doesn't need to know which —
// it's still just "does this id/seq pair match an outstanding probe I
// sent", the same as today.
std::optional<ParsedReply> parse_tcp_in_icmp_v4(const uint8_t* buf, size_t len) {
    if (len < 20) return std::nullopt;
    size_t ihl = static_cast<size_t>(buf[0] & 0x0f) * 4;
    if (len < ihl + 8) return std::nullopt;
    const uint8_t* icmp = buf + ihl;
    size_t icmplen = len - ihl;
    uint8_t t = icmp[0];
    if (t != 11 && t != 3) return std::nullopt; // only Time Exceeded / Unreachable carry an embedded original packet
    if (icmplen < 8) return std::nullopt;
    const uint8_t* inner = icmp + 8;
    size_t innerlen = icmplen - 8;
    if (innerlen < 20) return std::nullopt;
    size_t iihl = static_cast<size_t>(inner[0] & 0x0f) * 4;
    if (innerlen < iihl + 8) return std::nullopt; // need at least the embedded TCP header's first 8 bytes
    const uint8_t* tcp = inner + iihl;
    uint16_t src_port = be16(tcp);       // bytes 0-1
    uint16_t seq_low = be16(tcp + 6);    // low 16 bits of the 32-bit sequence number (bytes 4-7)
    return ParsedReply{t == 11 ? ReplyKind::TimeExceeded : ReplyKind::Unreachable, src_port, seq_low, 6, {}};
}

std::optional<ParsedReply> parse_tcp_in_icmp_v6(const uint8_t* buf, size_t len) {
    if (len < 8) return std::nullopt;
    uint8_t t = buf[0];
    if (t != 3 && t != 1) return std::nullopt; // ICMPv6 Time Exceeded / Destination Unreachable
    const uint8_t* inner = buf + 8;
    size_t innerlen = len - 8;
    if (innerlen < 40 + 8) return std::nullopt; // 40 (IPv6 header, no extension headers assumed) + embedded TCP's first 8 bytes
    const uint8_t* tcp = inner + 40;
    uint16_t src_port = be16(tcp);
    uint16_t seq_low = be16(tcp + 6);
    return ParsedReply{t == 3 ? ReplyKind::TimeExceeded : ReplyKind::Unreachable, src_port, seq_low, 6, {}};
}

// -------------------------------------------------------------- ProbeTcp

ProbeTcp::ProbeTcp(Family family, uint16_t dest_port, std::string source)
    : family_(family), dest_port_(dest_port), source_(std::move(source)) {
    ensure_winsock_ready();
}

ProbeTcp::~ProbeTcp() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [fd, p] : pending_) CLOSESOCK(static_cast<int>(fd));
}

uint32_t ProbeTcp::new_flow_pin() {
    // See allocate_flow_port_block()'s doc comment (probe.hpp) for why
    // this is no longer derived from this object's own address — that had
    // zero collision avoidance across simultaneous sessions (of the same
    // or different protocols), and a collision here is a real bug in the
    // reply-routing registry, not a tolerated coincidence.
    return allocate_flow_port_block();
}

bool ProbeTcp::send_hop_probe(const std::string& dest_ip, uint8_t ttl, uint16_t seq,
                               size_t /*payload*/, uint32_t pin) {
    return open_probe(dest_ip, ttl, seq, pin, /*is_direct=*/false);
}

bool ProbeTcp::send_direct_probe(const std::string& dest_ip, uint16_t seq,
                                  size_t /*payload*/, uint32_t pin) {
    // 255: same "always valid regardless of real path length" reasoning
    // as ProbeIcmp's kDirectEchoTtl — this connect() is meant to reach the
    // real destination, not stop partway.
    return open_probe(dest_ip, 255, seq, pin, /*is_direct=*/true);
}

bool ProbeTcp::open_probe(const std::string& dest_ip, uint8_t ttl, uint16_t seq, uint32_t pin, bool is_direct) {
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

    // ATTEMPTED FIX, REVERTED: tried bounding the OS's own TCP connect
    // timeout here (TCP_MAXRT on Windows / TCP_USER_TIMEOUT on Linux) to
    // explain a live report of a LAN device consistently measuring ~2050ms
    // over TCP (tightly clustered — not the wide scatter real jitter would
    // produce) plus isolated multi-second spikes elsewhere that a reference
    // tool never showed for the same path. The theory: an uncontrolled OS-
    // level SYN-retry schedule occasionally/consistently taking several
    // seconds before select() reports ANY readiness, reported as a
    // technically-real but misleading "success".
    //
    // Built the Linux side (TCP_USER_TIMEOUT) and tested it directly: a
    // genuinely-silent target (SYN sent and confirmed dropped via iptables,
    // packet counters checked) still took the FULL session-level timeout
    // (10s) to give up, not the ~2s this option was set to — the setsockopt
    // call had no measurable effect on this kernel/environment. That
    // directly contradicts the assumption the fix was built on, so it was
    // removed rather than kept on unverified faith — including the analogous
    // Windows TCP_MAXRT attempt, which could never be tested at all in this
    // environment and whose underlying premise just failed empirically on
    // the one platform that COULD be checked.
    //
    // The diagnosis itself (uncontrolled, occasionally very slow OS-level
    // connect completion) may still be correct — removing a non-working
    // mitigation doesn't retract the underlying observation — but the right
    // fix needs more investigation (possibly: shortening THIS tool's own
    // TCP timeout_secs specifically, rather than fighting the OS's at the
    // socket level; or logging enough detail — see the port-logging work
    // elsewhere this round — to see exactly what is happening on a real
    // Windows machine when this recurs) rather than a speculative one that
    // tested negative on the only platform available to verify it on.

    // TTL/hop-limit — this is what makes this connect() a HOP-DISCOVERY
    // probe rather than a direct one when ttl < 255, exactly analogous to
    // Prober::send()'s ttl parameter, just applied via setsockopt on a
    // real socket instead of an IP header field the caller builds by hand
    // (see this file's top comment for why that difference is
    // unavoidable on Windows).
    if (family_ == Family::V4) {
        int ttl_i = ttl;
        ::setsockopt(static_cast<int>(fd), IPPROTO_IP, IP_TTL, reinterpret_cast<const char*>(&ttl_i), sizeof(ttl_i));
    } else {
        int ttl_i = ttl;
        ::setsockopt(static_cast<int>(fd), IPPROTO_IPV6, IPV6_UNICAST_HOPS, reinterpret_cast<const char*>(&ttl_i), sizeof(ttl_i));
    }

    // Flow pin: bind the LOCAL port explicitly, same idea as ICMP's
    // checksum pin but structurally simpler — this isn't a workaround, a
    // fixed source port is the literal field real ECMP hashing uses (see
    // probe_tcp.hpp's top comment).
    //
    // IMPORTANT LIMITATION, found by actually running this against a live
    // target rather than just reading it: unlike ICMP (one shared socket,
    // many in-flight probes multiplexed by id/seq), a TCP probe is a real
    // OS-level (local_ip:local_port, dest_ip:dest_port) socket, and the OS
    // will not let two of those be simultaneously identical — a
    // hop-discovery probe and a direct probe in flight to the same
    // destination:port at the same pinned port collide with
    // EADDRNOTAVAIL/EADDRINUSE. So the pin can only be held FIXED across
    // probes that are never simultaneously outstanding — i.e. consistent
    // across successive rounds' probe of the SAME ttl (which is what
    // matters for comparing round N to round N+1), but NOT across
    // different ttls within one discovery round, since those genuinely
    // are simultaneous. Deriving the bound port from pin+ttl keeps
    // same-ttl probes pinned to each other round-over-round while giving
    // different ttls in the same round distinct, non-colliding ports.
    //
    // The real cost: within a single round, different hops CAN still hash
    // onto different ECMP branches, something ICMP's single-socket
    // checksum-pinning avoids entirely. This isn't fixable within this
    // connect()-based model (see this file's top comment on why raw SYN
    // crafting — which WOULD allow one truly fixed port across
    // simultaneous sends — doesn't work on Windows). The existing
    // loop-auditor/guarded-wipe machinery (ARCHITECTURE.md §5) is what
    // absorbs the consequence — it already tolerates ECMP variance by
    // design, TCP mode just asks more of it than ICMP mode does.
    int reuse = 1;
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (pin != 0) {
        uint16_t local_port = static_cast<uint16_t>((pin + ttl) & 0xFFFFu);
        if (local_port < 1024) local_port += 1024; // stay out of the privileged range after the offset
        bool bound = false;
        if (family_ == Family::V4) {
            sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(local_port);
            if (!source_.empty()) ::inet_pton(AF_INET, source_.c_str(), &local.sin_addr);
            bound = ::bind(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
        } else {
            sockaddr_in6 local{}; local.sin6_family = AF_INET6; local.sin6_port = htons(local_port);
            if (!source_.empty()) ::inet_pton(AF_INET6, source_.c_str(), &local.sin6_addr);
            bound = ::bind(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
        }
        // BUG FIX: the bind result used to be discarded outright, on the
        // reasoning that "connect() still works against whatever ephemeral
        // port the OS falls back to, so losing the pin for one probe is
        // cheaper than losing the probe." That is true for ECMP pinning —
        // but NOT for reply correlation, which is what actually breaks.
        // session.cpp registers (pin + ttl) as this probe's reply key and
        // maps an incoming ICMP Time-Exceeded back to its ttl by computing
        // (reply.id - pin). A silently-failed bind means the packet really
        // left from some unrelated ephemeral port, so the router's
        // Time-Exceeded quotes THAT port, the range check rejects it as
        // "not ours", and the hop never resolves — it just times out
        // forever while the destination (whose reply comes back through
        // poll_completions(), not the ICMP path) keeps working fine. That
        // is precisely the "destination responds but intermediate hops
        // never do" shape reported live, and it is NOT hypothetical on
        // Windows: binding in the 49152+ range there returns WSAEACCES
        // whenever Hyper-V/WSL2/Docker has reserved that block, which the
        // same app already hit and logged in UDP mode.
        //
        // Read back what the socket is ACTUALLY bound to and report that,
        // so the caller registers reality instead of an assumption. Same
        // getsockname()-confirmation approach ProbeUdp::new_flow_pin()
        // already uses.
        (void)bound; // the confirmed value below is what matters, not the attempt
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
        if (err != INPROGRESS && !ISCONNREFUSED(err)) {
            // A genuinely unrecoverable local error (e.g. no route at all,
            // distinct from a normal in-flight connection or a same-host
            // RST) — nothing worth tracking, this attempt never left.
            CLOSESOCK(static_cast<int>(fd));
            return false;
        }
        // In-progress (the overwhelmingly common case, non-blocking
        // connect) or immediate ECONNREFUSED (rare but valid — a
        // same-host or already-known-closed port can refuse before this
        // function even returns) both fall through to being tracked below;
        // poll_completions() distinguishes them by re-checking SO_ERROR.
    }

    // Confirm the real local port AFTER connect(). This must not happen
    // earlier: if the explicit bind() above failed, the socket is still
    // UNBOUND at that point and getsockname() reports port 0 — the OS only
    // assigns the fallback ephemeral port as part of connect(). Reading it
    // too early therefore yields 0 in exactly the failed-bind case this
    // exists to handle, which is precisely the wrong answer (verified by a
    // forced-collision test that returned 0 before this was moved here).
    uint16_t actual_port = 0;
    {
        sockaddr_storage sa{}; socklen_t sl = sizeof(sa);
        if (::getsockname(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&sa), &sl) == 0) {
            actual_port = (sa.ss_family == AF_INET)
                ? ntohs(reinterpret_cast<sockaddr_in*>(&sa)->sin_port)
                : ntohs(reinterpret_cast<sockaddr_in6*>(&sa)->sin6_port);
        }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    last_bound_port_.store(actual_port);
    pending_[fd] = Pending{fd, dest_ip, seq, now_secs_local(), now_mono_local(), is_direct};
    return true;
}

bool ProbeTcp::has_pending() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !pending_.empty();
}

std::vector<ProbeReply> ProbeTcp::poll_completions(double /*now*/, double timeout_secs) {
    const double mono_now = now_mono_local(); // durations only — see Pending::sent_at_mono
    // Deliberately ignoring the caller's `now` for anything time-sensitive
    // below — see this function's doc comment in probe_tcp.hpp update: a
    // caller's `now` captured at the top of ITS OWN loop iteration can
    // predate a probe this same iteration went on to send moments later
    // (sent_at is recorded fresh, right at send time). Using that stale
    // `now` for an RTT delta against a probe that completes near-instantly
    // within the same iteration produces a small negative RTT — exactly
    // the -0.1 to -0.6ms values seen in production. A fresh timestamp,
    // taken right here, can never predate any sent_at already recorded.
    const double now = now_secs_local();
    std::vector<ProbeReply> out;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = pending_.begin(); it != pending_.end();) {
        long long fd = it->first;
        Pending& p = it->second;

        fd_set wfds; FD_ZERO(&wfds); FD_SET(static_cast<int>(fd), &wfds);
        fd_set efds; FD_ZERO(&efds); FD_SET(static_cast<int>(fd), &efds);
        timeval tv{0, 0}; // non-blocking poll — this function is called from a poll loop, not blocking itself
        int r = ::select(static_cast<int>(fd) + 1, nullptr, &wfds, &efds, &tv);

        bool writable = r > 0 && FD_ISSET(static_cast<int>(fd), &wfds);
        bool errored = r > 0 && FD_ISSET(static_cast<int>(fd), &efds);
        if (writable || errored) {
            int so_err = 0; socklen_t so_len = sizeof(so_err);
            ::getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &so_len);
            if (so_err == 0) {
                // connect() completed cleanly — SYN-ACK arrived, handshake
                // finished. Destination answered: always DirectReply, even
                // for a probe sent with ttl < 255 — see Pending::is_direct's
                // doc comment in probe_tcp.hpp.
                //
                // Force an ABORTIVE close (RST) instead of the default
                // graceful one (FIN) before this socket gets closed below —
                // see this file's own history: a graceful close on a fully
                // ESTABLISHED connection puts the LOCAL PORT into TIME_WAIT
                // (commonly 30s-4min depending on OS), and this exact port
                // (pin+ttl) gets reused on every subsequent round for the
                // same TTL. Without this, a hop that ever actually reaches
                // the real destination — the only case that completes a
                // full handshake, unlike a Time-Exceeded or RST — would
                // degrade after its very first success, unable to reliably
                // rebind its port for the next round's probe.
                linger lg{1, 0};
                ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
                out.push_back(ProbeReply{ProbeOutcome::DirectReply, p.dest_ip, now, (mono_now - p.sent_at_mono) * 1000.0, p.seq, {}, true});
            } else if (ISCONNREFUSED(so_err)) {
                // RST — the destination is there and answered, it just
                // isn't listening on this port. Still a valid direct reply
                // for measurement purposes (same convention classic TCP
                // ping tools use: a refused connection proves reachability).
                out.push_back(ProbeReply{ProbeOutcome::DirectReply, p.dest_ip, now, (mono_now - p.sent_at_mono) * 1000.0, p.seq, {}, false});
            }
            // Any other error: genuinely unreachable from this socket's own
            // perspective — no ProbeReply pushed. If this was a
            // hop-discovery probe (ttl < 255) and a router along the way
            // actually TTL-expired it, the real signal already arrived
            // separately via the shared ICMP path (see this file's top
            // comment) — this socket's own failure is not itself news.
            CLOSESOCK(static_cast<int>(fd));
            it = pending_.erase(it);
            continue;
        }
        if (mono_now - p.sent_at_mono > timeout_secs) {
            // Gave it long enough — clean up. Same reasoning as above: no
            // reply from THIS socket by now just means no reply from this
            // socket, not necessarily an outage (a hop-discovery probe's
            // real answer, if any, came via ICMP already).
            CLOSESOCK(static_cast<int>(fd));
            it = pending_.erase(it);
            continue;
        }
        ++it;
    }
    return out;
}

} // namespace netpulse

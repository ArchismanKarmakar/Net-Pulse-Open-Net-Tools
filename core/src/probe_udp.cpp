#include "netpulse/probe_udp.hpp"
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
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>
#  define CLOSESOCK ::close
#  define LASTERR errno
#endif

namespace netpulse {

static inline uint16_t be16(const uint8_t* b) { return static_cast<uint16_t>((b[0] << 8) | b[1]); }

// ------------------------------------------------ embedded-UDP parsing

std::optional<UdpEmbeddedReply> parse_udp_in_icmp_v4(const uint8_t* buf, size_t len) {
    if (len < 20) return std::nullopt;
    size_t ihl = static_cast<size_t>(buf[0] & 0x0f) * 4;
    if (len < ihl + 8) return std::nullopt;
    const uint8_t* icmp = buf + ihl;
    size_t icmplen = len - ihl;
    uint8_t t = icmp[0];
    uint8_t code = icmp[1];
    if (t != 11 && t != 3) return std::nullopt; // only Time Exceeded / Unreachable carry an embedded original packet
    if (icmplen < 8) return std::nullopt;
    const uint8_t* inner = icmp + 8;
    size_t innerlen = icmplen - 8;
    if (innerlen < 20) return std::nullopt;
    size_t iihl = static_cast<size_t>(inner[0] & 0x0f) * 4;
    if (innerlen < iihl + 8) return std::nullopt; // need the embedded UDP header's first 8 bytes (src/dst port, length, checksum)
    const uint8_t* udp = inner + iihl;
    UdpEmbeddedReply r;
    r.kind = (t == 11) ? ReplyKind::TimeExceeded : ReplyKind::Unreachable;
    r.dest_port_unreachable = (t == 3 && code == 3); // ICMPv4 code 3 == port unreachable specifically
    r.src_port = be16(udp);
    r.dst_port = be16(udp + 2);
    return r;
}

std::optional<UdpEmbeddedReply> parse_udp_in_icmp_v6(const uint8_t* buf, size_t len) {
    if (len < 8) return std::nullopt;
    uint8_t t = buf[0];
    uint8_t code = buf[1];
    if (t != 3 && t != 1) return std::nullopt; // ICMPv6 Time Exceeded / Destination Unreachable
    const uint8_t* inner = buf + 8;
    size_t innerlen = len - 8;
    if (innerlen < 40 + 8) return std::nullopt;
    const uint8_t* udp = inner + 40;
    UdpEmbeddedReply r;
    r.kind = (t == 3) ? ReplyKind::TimeExceeded : ReplyKind::Unreachable;
    r.dest_port_unreachable = (t == 1 && code == 4); // ICMPv6 code 4 == port unreachable specifically
    r.src_port = be16(udp);
    r.dst_port = be16(udp + 2);
    return r;
}

// -------------------------------------------------------------- ProbeUdp

ProbeUdp::ProbeUdp(Family family, uint16_t dest_port, std::string source)
    : family_(family), dest_port_(dest_port), source_(std::move(source)) {
    ensure_winsock_ready();
    int af = (family_ == Family::V4) ? AF_INET : AF_INET6;
    fd_ = static_cast<long long>(::socket(af, SOCK_DGRAM, IPPROTO_UDP));
    if (fd_ < 0) { error_ = "socket() failed"; return; }
#ifdef _WIN32
    // Windows, and only Windows, propagates an ICMP error triggered by a
    // UDP send back to that SAME socket's next call (send or receive),
    // which then fails with WSAECONNRESET — a well-known, long-documented
    // quirk (unrelated to whether the socket is connect()ed; it applies
    // here too). UDP traceroute's entire mechanism is DELIBERATELY
    // eliciting exactly this kind of ICMP error (Port-Unreachable from
    // the destination once the probe reaches it) — and since one
    // persistent socket serves every probe this session ever sends (see
    // new_flow_pin()'s doc comment for why that's correct), the very
    // first time the mechanism successfully works, it would poison every
    // subsequent sendto() on this socket, forever. The standard, official
    // fix is this ioctl — SIO_UDP_CONNRESET with FALSE turns the
    // propagation off. SIO_UDP_CONNRESET isn't always available via a
    // simple header include depending on the SDK/toolchain in use, so it's
    // defined directly here (a stable, documented constant, not
    // version-fragile) rather than depending on mswsock.h being present.
#  ifndef SIO_UDP_CONNRESET
#    define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#  endif
    BOOL new_behavior = FALSE;
    DWORD bytes_returned = 0;
    ::WSAIoctl(static_cast<SOCKET>(fd_), SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
               nullptr, 0, &bytes_returned, nullptr, nullptr);
    // Deliberately not checking this call's own return value — if it
    // somehow fails, the socket still works for the general case (this
    // only ever mattered for the specific "already got one ICMP error on
    // this socket" scenario), so there's nothing more useful to do with a
    // failure here than silently proceeding.
#endif
}

ProbeUdp::~ProbeUdp() {
    if (fd_ >= 0) CLOSESOCK(static_cast<int>(fd_));
}

// See this method's declaration (probe_udp.hpp) for the full rationale.
// Always logged when the fallback path is taken (not gated behind
// NETPULSE_DEBUG) — this should be a rare event in a healthy run, and when
// it does happen it's exactly the kind of thing that otherwise turns into
// an unexplained "UDP mode never receives anything" report with nothing
// to point at.
uint16_t ProbeUdp::bind_and_confirm_port(uint16_t candidate_port) {
    auto try_bind = [&](uint16_t port) -> bool {
        if (family_ == Family::V4) {
            sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(port);
            if (!source_.empty()) ::inet_pton(AF_INET, source_.c_str(), &local.sin_addr);
            return ::bind(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
        } else {
            sockaddr_in6 local{}; local.sin6_family = AF_INET6; local.sin6_port = htons(port);
            if (!source_.empty()) ::inet_pton(AF_INET6, source_.c_str(), &local.sin6_addr);
            return ::bind(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0;
        }
    };

    bool bound = try_bind(candidate_port);
    if (!bound) {
        int bind_errno = LASTERR;
        fprintf(stderr, "[netpulse-udp] bind() to candidate port %u failed (errno %d) — "
                         "falling back to an OS-assigned port. Correlation will still work "
                         "correctly (this session registers whatever port actually gets bound, "
                         "not the original candidate), but this is worth knowing about if it "
                         "happens often: it usually means something else on this machine is "
                         "already using ports in the 49152+ range this app draws candidates "
                         "from.\n", (unsigned)candidate_port, bind_errno);
        bound = try_bind(0); // port 0 = "OS, pick any free one" — should essentially never fail
        if (!bound) {
            fprintf(stderr, "[netpulse-udp] fallback bind() to port 0 ALSO failed (errno %d) — "
                             "this socket cannot be bound at all; sends may still nominally "
                             "succeed but replies can never be correlated back to this session.\n", LASTERR);
            return 0;
        }
    }

    // Read back whatever port is ACTUALLY bound now, regardless of which
    // branch above got us here — this is the one value that's actually
    // trustworthy, as opposed to assuming the candidate (or even port 0's
    // *intent*) reflects reality.
    if (family_ == Family::V4) {
        sockaddr_in actual{}; socklen_t len = sizeof(actual);
        if (::getsockname(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&actual), &len) != 0) return 0;
        return ntohs(actual.sin_port);
    } else {
        sockaddr_in6 actual{}; socklen_t len = sizeof(actual);
        if (::getsockname(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&actual), &len) != 0) return 0;
        return ntohs(actual.sin6_port);
    }
}

uint32_t ProbeUdp::new_flow_pin() {
    // See allocate_flow_port_block()'s doc comment (probe.hpp) — no longer
    // derived from this object's own address, for the same collision-risk
    // reason ProbeTcp's new_flow_pin() was changed.
    //
    // BUG FIX: this used to just return the candidate port number, with
    // the actual bind() deferred to send_at()'s first call — and that
    // bind()'s return value was never checked at all. If it failed (a real
    // possibility: candidates are drawn from the exact same 49152+ range
    // Windows itself hands out ephemeral ports from for unrelated sockets
    // across the whole system, so a collision with something else already
    // using that specific port is entirely plausible, not exotic), the
    // socket would silently end up bound to whatever port the OS happened
    // to fall back to instead — while this session had already registered
    // (and every subsequent embedded-reply match against) the ORIGINAL,
    // never-actually-used candidate. Every real reply for this session's
    // probes would then carry the wrong embedded source port, get
    // filtered out as "not this session's probe" before ever being
    // counted, and this session would receive literally nothing, forever,
    // with no error anywhere — exactly the "sends succeed, outstanding
    // cycles normally, zero replies ever, not even a mismatched one"
    // shape a live debug session confirmed.
    //
    // Binding now happens HERE, eagerly, before this pin is ever handed to
    // the caller (session.cpp registers it immediately via
    // register_icmp_owner) — see bind_and_confirm_port()'s doc comment for
    // the full fallback-and-verify mechanism. Whatever it returns is
    // guaranteed to be the port this socket is actually bound to, so the
    // registry is keyed on reality instead of an unverified intent.
    uint16_t candidate = static_cast<uint16_t>(allocate_flow_port_block());
    uint16_t actual = bind_and_confirm_port(candidate);
    bound_to_pin_ = true; // either branch inside bind_and_confirm_port already resolved this — send_at()'s own lazy path below is now purely a defensive fallback for a caller that skipped new_flow_pin() entirely
    return actual;
}

bool ProbeUdp::send_hop_probe(const std::string& dest_ip, uint8_t ttl, uint16_t /*seq*/,
                               size_t payload, uint32_t pin) {
    return send_at(dest_ip, ttl, payload, pin);
}

bool ProbeUdp::send_direct_probe(const std::string& dest_ip, uint16_t /*seq*/,
                                  size_t payload, uint32_t pin) {
    return send_at(dest_ip, 255, payload, pin);
}

// See this method's declaration (probe_udp.hpp) for why a genuine per-probe
// UDP ping needs a DIFFERENT correlation scheme than traceroute's: a ping
// always wants the real, full IP TTL (255 — reach the destination directly,
// not deliberately expire partway there), but still needs something that
// varies per probe for the reply to be matched back to a specific sequence
// number, the same problem send_at()'s dest_port_+ttl trick solves for
// traceroute — just with ttl no longer available to serve double duty here.
// Wraps modulo kPingPortWindow (not raw seq) so a long-running or continuous
// ping doesn't walk dest_port_ past the valid 16-bit port range; kept
// generous relative to how many probes are realistically ever outstanding
// at once (interval-gated, not a traceroute-style simultaneous burst), so
// a wrapped-around reply colliding with a still-genuinely-outstanding
// earlier probe is not a realistic concern.
bool ProbeUdp::send_ping_probe(const std::string& dest_ip, uint16_t seq, size_t payload, uint32_t pin) {
    uint16_t probe_dest_port = static_cast<uint16_t>(dest_port_ + (seq % kPingPortWindow));
    return send_raw(dest_ip, 255, probe_dest_port, payload, pin);
}

bool ProbeUdp::send_at(const std::string& dest_ip, uint8_t ttl, size_t payload, uint32_t pin) {
    // Destination port varies per TTL (dest_port_ + ttl) — this is the
    // actual correlation mechanism, not just historical convention: this
    // session's SOURCE port is fixed for its entire lifetime (see
    // new_flow_pin()'s doc comment), so without something else varying,
    // every probe would look identical from a reply's perspective and
    // there'd be no way to tell which TTL a given Time-Exceeded/
    // Unreachable was actually answering. The embedded original's
    // destination port survives back to the caller as ParsedReply::seq
    // (icmp.cpp's protocol-sniffing parser) — recover the TTL with
    // `ttl = seq - dest_port_`. A direct probe (ttl 255) still gets its
    // own distinct port (dest_port_ + 255) for the same reason.
    uint16_t probe_dest_port = static_cast<uint16_t>(dest_port_ + ttl);
    return send_raw(dest_ip, ttl, probe_dest_port, payload, pin);
}

bool ProbeUdp::send_raw(const std::string& dest_ip, uint8_t ip_ttl, uint16_t probe_dest_port,
                         size_t payload, uint32_t pin) {
    if (fd_ < 0) return false;
    std::lock_guard<std::mutex> lk(send_mtx_);

    // Normal callers (session.cpp) always call new_flow_pin() first, which
    // now performs and confirms this binding eagerly — see its own doc
    // comment for why. bound_to_pin_ will already be true by the time any
    // real send reaches here. This block is purely a defensive fallback
    // for a caller that somehow invoked send_hop_probe()/send_direct_probe()
    // with a nonzero pin WITHOUT calling new_flow_pin() first — uses the
    // same verified bind_and_confirm_port() path rather than the previous
    // fire-and-forget bind() with no return-value check, for the same
    // correctness reason. Note this fallback path can't retroactively fix
    // up whatever the caller already registered in the reply-correlation
    // table if the actually-bound port ends up different from `pin` — that
    // guarantee only holds for the normal new_flow_pin()-first path.
    if (!bound_to_pin_ && pin != 0) {
        bind_and_confirm_port(static_cast<uint16_t>(pin & 0xFFFFu));
        bound_to_pin_ = true; // don't retry every send — one attempt is enough
    }

    // BUG FIX: same class of problem as bind() above — the return value
    // here was previously discarded entirely. If this setsockopt silently
    // fails, EVERY probe leaves at the socket's default TTL (commonly 128)
    // instead of the intended 1..max_hops value — meaning every single
    // probe sails straight past every intermediate router without ever
    // expiring, reaching the destination directly regardless of which
    // "ttl" the caller asked for. That would produce EXACTLY the pattern
    // debug logs have shown: not even the nearest hop (normally the local
    // router, milliseconds away, always the first and most reliable reply
    // in ICMP mode) ever generates a Time-Exceeded, because its TTL never
    // actually expires there. Whatever happens after that depends entirely
    // on whether the real destination chooses to answer an arbitrary UDP
    // packet with an ICMP error — plenty of real infrastructure (Google's
    // public DNS among it) just silently drops one instead. Logged once
    // per session (not every probe — this would otherwise fire up to
    // max_hops times per second) so a persistent failure here is visible
    // without flooding the log for a single transient one.
    bool ttl_ok;
    if (family_ == Family::V4) {
        int ttl_i = ip_ttl;
        ttl_ok = ::setsockopt(static_cast<int>(fd_), IPPROTO_IP, IP_TTL, reinterpret_cast<const char*>(&ttl_i), sizeof(ttl_i)) == 0;
    } else {
        int ttl_i = ip_ttl;
        ttl_ok = ::setsockopt(static_cast<int>(fd_), IPPROTO_IPV6, IPV6_UNICAST_HOPS, reinterpret_cast<const char*>(&ttl_i), sizeof(ttl_i)) == 0;
    }
    if (!ttl_ok && !warned_ttl_failure_) {
        warned_ttl_failure_ = true;
        fprintf(stderr, "[netpulse-udp] setsockopt(TTL=%u) failed (errno %d) — probes will leave at "
                         "whatever this socket's default TTL is instead of the intended per-hop value, "
                         "meaning every probe reaches the destination directly and no intermediate hop "
                         "will ever be discovered. This message only prints once per session even if it "
                         "keeps failing.\n", (unsigned)ip_ttl, LASTERR);
    }

    sockaddr_storage dst{};
    socklen_t dst_len = 0;
    if (family_ == Family::V4) {
        auto* d4 = reinterpret_cast<sockaddr_in*>(&dst);
        d4->sin_family = AF_INET; d4->sin_port = htons(probe_dest_port);
        if (::inet_pton(AF_INET, dest_ip.c_str(), &d4->sin_addr) != 1) return false;
        dst_len = sizeof(*d4);
    } else {
        auto* d6 = reinterpret_cast<sockaddr_in6*>(&dst);
        d6->sin6_family = AF_INET6; d6->sin6_port = htons(probe_dest_port);
        if (::inet_pton(AF_INET6, dest_ip.c_str(), &d6->sin6_addr) != 1) return false;
        dst_len = sizeof(*d6);
    }

    std::vector<uint8_t> body(payload, 0xA5); // same filler convention as icmp.cpp's build_echo
    int rc = ::sendto(static_cast<int>(fd_), reinterpret_cast<const char*>(body.data()), static_cast<int>(body.size()), 0,
                       reinterpret_cast<sockaddr*>(&dst), dst_len);
    if (rc < 0) last_send_errno_ = LASTERR;
    return rc >= 0;
}

} // namespace netpulse

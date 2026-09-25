// UDP ProbeStrategy — structurally simpler than probe_tcp.hpp's, for two
// reasons worth calling out explicitly:
//
// 1. No Windows restriction analogous to TCP's blocked raw SYN. An
//    ordinary (non-raw) SOCK_DGRAM socket has always been fully able to
//    send an arbitrary UDP datagram on any OS — there's no OS-level
//    anti-spoofing rule to route around here, so this can use ONE
//    persistent socket per session, exactly like ICMP's Prober, instead
//    of TCP's pool of short-lived per-probe sockets.
//
// 2. EVERY reply — both intermediate-hop AND the final destination's own
//    reply — arrives via ICMP, so this class doesn't need any
//    completion-polling machinery at all. Classic Unix traceroute's whole
//    trick is deliberately targeting a high, unlikely-to-be-listened UDP
//    port: the destination itself replies with ICMP Port Unreachable
//    (type 3, code 3) once nothing intercepts it before the datagram is
//    delivered, and every intermediate router that TTL-expires it along
//    the way replies with the same ICMP Time-Exceeded any other protocol
//    gets (IP-layer behavior, not transport-layer — see probe_tcp.hpp's
//    equivalent comment). So unlike TCP, there is no protocol-specific
//    "did this probe succeed" signal to poll for locally at all — 100% of
//    the answer comes back through the same shared ICMP raw socket /
//    RX dispatcher every mode already uses, just needing this file's
//    parse_udp_in_icmp_v4/v6 to recognize the embedded UDP header and
//    (uniquely among the three parsers so far) to distinguish ordinary
//    Time-Exceeded from the specific "port unreachable" Unreachable code
//    that means "the destination itself answered", not just some hop.
#pragma once
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "netpulse/icmp.hpp"
#include "netpulse/probe.hpp"

namespace netpulse {

// Same shape as parse_tcp_in_icmp_v4/v6 (probe_tcp.hpp) but reading a UDP
// header's layout: source port + dest port (bytes 0-3), then length +
// checksum (bytes 4-7) — NOT a sequence number, unlike TCP's embedded
// header at the same offset. `id` is the embedded source port (the
// matching key — see ProbeUdp::new_flow_pin()'s doc comment for why
// source port ALONE is sufficient to identify which probe this was,
// without needing anything from bytes 4-7). `seq` here is the embedded
// DEST port instead of a fabricated sequence number — a real, meaningful
// field, just not literally "sequence" semantics; kept for attribution
// (e.g. confirming the reply really is about a probe this session sent
// to the port it configured), not required for matching.
//
// `kind` additionally distinguishes ICMP Unreachable code 3 (port
// unreachable — the destination itself answered: ProbeOutcome::DirectReply)
// from any other Unreachable code (network/host/protocol unreachable —
// still worth surfacing, but NOT proof the destination itself was
// reached, unlike port-unreachable specifically) via the `dest_port_unreachable`
// flag, since ReplyKind::Unreachable alone doesn't carry that distinction
// today (icmp.hpp's existing ReplyKind was never asked to before).
struct UdpEmbeddedReply {
    ReplyKind kind;
    bool dest_port_unreachable = false; // true only for ICMPv4 code 3 / ICMPv6 code 4 specifically
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
};
std::optional<UdpEmbeddedReply> parse_udp_in_icmp_v4(const uint8_t* buf, size_t len);
std::optional<UdpEmbeddedReply> parse_udp_in_icmp_v6(const uint8_t* buf, size_t len);

class ProbeUdp : public ProbeStrategy {
public:
    // `dest_port` defaults to 33434 — the classic Unix traceroute base
    // port, chosen historically for the same reason this needs it:
    // vanishingly unlikely to have anything actually listening, so the
    // destination reliably answers with Port Unreachable instead of real
    // application data.
    ProbeUdp(Family family, uint16_t dest_port = 33434, std::string source = std::string());
    ~ProbeUdp() override;
    ProbeUdp(const ProbeUdp&) = delete;
    ProbeUdp& operator=(const ProbeUdp&) = delete;

    const char* name() const override { return "udp"; }
    bool ok() const { return fd_ >= 0; }
    const std::string& error() const { return error_; }
    // The last OS-level error code from a failed sendto() (0 if none, or
    // if the most recent send succeeded) — WSAGetLastError() value on
    // Windows, errno on POSIX. Exists purely for diagnostics: when a
    // report says "sends aren't working" with no other information, this
    // is what actually tells you why, instead of guessing further.
    int last_send_errno() const { return last_send_errno_; }

    bool send_hop_probe(const std::string& dest_ip, uint8_t ttl, uint16_t seq,
                         size_t payload, uint32_t pin) override;
    bool send_direct_probe(const std::string& dest_ip, uint16_t seq,
                            size_t payload, uint32_t pin) override;

    // A genuine per-probe UDP ping — unlike send_direct_probe() (a single
    // fixed-port traceroute-completion probe), this is meant to be called
    // repeatedly, once per sequence number, the way PingRun's ICMP path
    // already does. See this method's definition (probe_udp.cpp) for why
    // send_at()'s dest_port_+ttl correlation trick doesn't fit here: a ping
    // always wants the real, full IP TTL, so ttl isn't available to also
    // carry per-probe identity the way it does for traceroute. `seq` here
    // plays that role instead, wrapped modulo kPingPortWindow to stay
    // within a valid destination port.
    bool send_ping_probe(const std::string& dest_ip, uint16_t seq, size_t payload, uint32_t pin);
    static constexpr uint16_t kPingPortWindow = 4096; // see send_ping_probe()'s doc comment

    // Unlike TCP, this can be a single value fixed for the session's
    // entire lifetime — see this file's top comment: an UNCONNECTED UDP
    // socket has none of connect()'s "identical 4-tuple can't be
    // simultaneously active twice" restriction (that constraint is
    // specifically a property of CONNECTED sockets — TCP's, or a UDP
    // socket that called connect() itself, neither of which apply here),
    // so every probe this session ever sends, at any TTL, in flight
    // simultaneously or not, can share the exact same source port. This
    // is the closest of the three implementations so far to ICMP's own
    // one-fixed-pin-per-session guarantee — no probe_tcp.hpp-style
    // per-round pinning compromise needed.
    uint32_t new_flow_pin() override;

private:
    // send_at() (traceroute: ip_ttl and the correlation port are the same
    // value) and send_ping_probe() (ping: ip_ttl fixed at 255, correlation
    // is a separate value) both funnel through this — see send_raw()'s own
    // definition for the shared bind/setsockopt/sendto mechanics neither
    // needs to duplicate.
    bool send_at(const std::string& dest_ip, uint8_t ttl, size_t payload, uint32_t pin);
    bool send_raw(const std::string& dest_ip, uint8_t ip_ttl, uint16_t probe_dest_port,
                  size_t payload, uint32_t pin);
    // BUG FIX (see this method's definition, probe_udp.cpp, for the full
    // story): binds to `candidate_port`, but no longer trusts that binding
    // silently — verifies the actual result with getsockname() and, if the
    // requested port couldn't be bound (already in use is entirely
    // plausible: this app's own candidate ports are drawn from the SAME
    // 49152+ dynamic range Windows itself hands out ephemeral ports from
    // for completely unrelated sockets across the whole system), falls
    // back to binding port 0 (any free port the OS picks) and reads back
    // WHATEVER port that actually turned out to be. Returns the port this
    // socket is really bound to, which is what must be used for the
    // reply-correlation registry (register_icmp_owner, session.cpp) — NOT
    // necessarily `candidate_port`. Returns 0 only if binding failed
    // outright even for the port-0 fallback (should be exceedingly rare;
    // that's an OS-level socket problem, not a "this one port was busy"
    // problem).
    uint16_t bind_and_confirm_port(uint16_t candidate_port);

    Family family_;
    uint16_t dest_port_;
    std::string source_;
    long long fd_ = -1;
    std::string error_;
    std::mutex send_mtx_; // serializes {setsockopt TTL, sendto} — same reasoning as Prober's send_mtx_ (transport.hpp)
    bool bound_to_pin_ = false; // true once bind() has succeeded for a given pin — see send_at()'s doc comment
    bool warned_ttl_failure_ = false; // see send_at()'s TTL setsockopt doc comment — log once per session, not once per probe
    int last_send_errno_ = 0; // see last_send_errno()'s doc comment
};

} // namespace netpulse

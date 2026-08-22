// ICMP / ICMPv6 echo construction and reply parsing (both address families).
// Mirrors the verified Rust core. The fiddly part is recovering the embedded
// probe from ICMP error messages (Time Exceeded / Unreachable).
#pragma once
#include <cstdint>
#include <cstddef>
#include <optional>
#include <vector>

namespace netpulse {

enum class Family { V4, V6 };
enum class ReplyKind { EchoReply, TimeExceeded, Unreachable };

struct ParsedReply {
    ReplyKind kind;
    uint16_t id;
    uint16_t seq;
    // Which protocol the EMBEDDED original packet was, for TimeExceeded/
    // Unreachable — 1=ICMP, 6=TCP, 17=UDP (standard IP protocol numbers),
    // meaningless (left at 1) for EchoReply, which has no embedded packet.
    // Exists because id/seq are read from different byte offsets/meanings
    // depending on this: an ICMP-embedded original's id/seq is its echo
    // identifier/sequence; a TCP-embedded original's id/seq is its source
    // port / low 16 bits of its sequence number; a UDP-embedded original's
    // id/seq is its source port / destination port. See parse_v4/parse_v6's
    // implementation (icmp.cpp) — this one raw-ICMP-socket dispatch path is
    // now shared by every protocol's sessions (see probe_udp.hpp's top
    // comment for why UDP mode's hop AND destination replies both arrive
    // here), so the parser sniffs the embedded IP header's protocol field
    // rather than assuming ICMP.
    uint8_t orig_proto = 1;
    // RFC 4950 MPLS label stack from a Time Exceeded/Unreachable's RFC 4884
    // extension structure, top-of-stack first. Each entry is the raw 32-bit
    // MPLS shim value: label:20 | exp/tc:3 | S:1 | ttl:8. Empty when the
    // reply carried no (or an unparseable) extension structure — most
    // routers along a real path never attach one.
    std::vector<uint32_t> mpls_labels;
};

// Internet checksum (RFC 1071).
uint16_t checksum(const uint8_t* data, size_t len);

// Build an ICMP echo request (no IP header). ICMPv6 checksum is left 0 — the
// kernel fills it for raw ICMPv6 sockets.
//
// `pin_checksum`, when set (IPv4 only — ignored for V6 since the kernel owns
// the checksum there), forces the packet's FINAL checksum to that exact fixed
// value regardless of `seq`, by writing an adjustment into the last 2 payload
// bytes (needs payload_len >= 2; silently skipped otherwise — the packet is
// still built normally, just without the pinning property). This is the
// "Paris traceroute" flow-pinning trick: routers that hash ECMP path
// selection on the ICMP checksum will then route every probe in a session
// down the SAME physical path, since they all present the same checksum —
// eliminating ECMP path variance as a source of false "same IP at two
// different hop counts" signals. See the loop-auditor comment in
// core/src/session.cpp for the full rationale.
std::vector<uint8_t> build_echo(Family f, uint16_t id, uint16_t seq, size_t payload_len,
                                 std::optional<uint16_t> pin_checksum = std::nullopt);

// Parse a buffer from a raw IPv4 socket (includes the IPv4 header).
std::optional<ParsedReply> parse_v4(const uint8_t* buf, size_t len);

// Parse a buffer from a raw IPv6 socket (kernel strips the IPv6 header).
std::optional<ParsedReply> parse_v6(const uint8_t* buf, size_t len);

} // namespace netpulse

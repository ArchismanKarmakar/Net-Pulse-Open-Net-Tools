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
};

// Internet checksum (RFC 1071).
uint16_t checksum(const uint8_t* data, size_t len);

// Build an ICMP echo request (no IP header). ICMPv6 checksum is left 0 — the
// kernel fills it for raw ICMPv6 sockets.
std::vector<uint8_t> build_echo(Family f, uint16_t id, uint16_t seq, size_t payload_len);

// Parse a buffer from a raw IPv4 socket (includes the IPv4 header).
std::optional<ParsedReply> parse_v4(const uint8_t* buf, size_t len);

// Parse a buffer from a raw IPv6 socket (kernel strips the IPv6 header).
std::optional<ParsedReply> parse_v6(const uint8_t* buf, size_t len);

} // namespace netpulse

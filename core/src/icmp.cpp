#include "netpulse/icmp.hpp"

namespace netpulse {

static inline uint16_t be16(const uint8_t* b) {
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
}

// RFC 4884 extension structure walk, shared by parse_v4/parse_v6. `icmp`
// points at the start of the ICMP/ICMPv6 header of a Time Exceeded/
// Unreachable message (byte 0 = type); `icmplen` is the remaining buffer
// length from there. RFC 4884 repurposes byte 5 of the classic 4-byte
// "unused" field as the length of the copied original datagram, in 4-byte
// words — a router that doesn't implement RFC 4884 leaves it 0, which is
// indistinguishable from "no extension present", so that case is treated as
// no labels rather than guessing an offset. Any malformed/truncated/
// checksum-mismatched structure also yields no labels — same fail-soft
// posture as the rest of this file, never UB.
static std::vector<uint32_t> parse_mpls_extensions(const uint8_t* icmp, size_t icmplen) {
    std::vector<uint32_t> labels;
    if (icmplen < 8) return labels;
    uint8_t orig_len_words = icmp[5];
    if (orig_len_words == 0) return labels; // legacy router, no RFC 4884 hint
    size_t ext_off = 8 + static_cast<size_t>(orig_len_words) * 4;
    if (ext_off + 4 > icmplen) return labels;
    const uint8_t* ext = icmp + ext_off;
    size_t ext_len = icmplen - ext_off;
    if ((ext[0] >> 4) != 2) return labels; // extension structure version must be 2
    if (checksum(ext, ext_len) != 0) return labels; // structure's own checksum must validate

    size_t pos = 4; // skip the 4-byte common header
    while (pos + 4 <= ext_len) {
        uint16_t obj_len = be16(ext + pos);
        if (obj_len < 4 || pos + obj_len > ext_len) break;
        uint8_t class_num = ext[pos + 2];
        uint8_t c_type = ext[pos + 3];
        if (class_num == 1 && c_type == 1) { // MPLS Label Stack Object (RFC 4950)
            size_t p = pos + 4;
            size_t obj_end = pos + obj_len;
            while (p + 4 <= obj_end) {
                uint32_t v = (static_cast<uint32_t>(ext[p]) << 24) | (static_cast<uint32_t>(ext[p + 1]) << 16) |
                             (static_cast<uint32_t>(ext[p + 2]) << 8) | static_cast<uint32_t>(ext[p + 3]);
                labels.push_back(v);
                p += 4;
            }
        }
        pos += obj_len;
    }
    return labels;
}

uint16_t checksum(const uint8_t* d, size_t len) {
    uint32_t sum = 0;
    size_t i = 0;
    for (; i + 1 < len; i += 2) {
        sum += static_cast<uint16_t>((d[i] << 8) | d[i + 1]);
    }
    if (i < len) {
        sum += static_cast<uint16_t>(d[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

// One's-complement addition with end-around-carry fold — the same fold rule
// checksum() uses internally. X + ~X always folds to exactly 0xFFFF for any
// 16-bit X (since ~X == 0xFFFF - X bitwise), which is the algebraic identity
// build_echo()'s checksum-pinning solves with below.
static inline uint16_t ones_complement_add(uint16_t a, uint16_t b) {
    uint32_t sum = static_cast<uint32_t>(a) + static_cast<uint32_t>(b);
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return static_cast<uint16_t>(sum);
}

std::vector<uint8_t> build_echo(Family f, uint16_t id, uint16_t seq, size_t payload,
                                 std::optional<uint16_t> pin_checksum) {
    uint8_t type = (f == Family::V4) ? 8 : 128;
    std::vector<uint8_t> p;
    p.reserve(8 + payload);
    p.push_back(type);
    p.push_back(0);
    p.push_back(0);
    p.push_back(0);
    p.push_back(static_cast<uint8_t>(id >> 8));
    p.push_back(static_cast<uint8_t>(id & 0xff));
    p.push_back(static_cast<uint8_t>(seq >> 8));
    p.push_back(static_cast<uint8_t>(seq & 0xff));
    p.insert(p.end(), payload, 0xA5);
    if (f == Family::V4) {
        if (pin_checksum && payload >= 2) {
            // Paris-traceroute-style flow pinning (see icmp.hpp doc comment).
            // The last 2 payload bytes are the adjustment field. Solve for the
            // adjustment value that forces the checksum recomputed below to
            // equal `*pin_checksum` exactly, regardless of what `seq` is:
            //   c0  = checksum(packet with adjustment=0, checksum field=0)
            //   adj = (~target) +1's-complement+ c0
            // Verified algebraically: recomputing checksum with `adj` written
            // in always yields exactly `*pin_checksum` for any c0 (i.e. any
            // seq) — see the derivation in the Pillar 3 plan notes.
            size_t adj_off = p.size() - 2;
            p[adj_off] = 0; p[adj_off + 1] = 0; // zero the adjustment field first
            p[2] = 0; p[3] = 0;                 // zero checksum field, as usual
            uint16_t c0 = checksum(p.data(), p.size());
            uint16_t adj = ones_complement_add(static_cast<uint16_t>(~(*pin_checksum)), c0);
            p[adj_off] = static_cast<uint8_t>(adj >> 8);
            p[adj_off + 1] = static_cast<uint8_t>(adj & 0xff);
        }
        uint16_t ck = checksum(p.data(), p.size());
        p[2] = static_cast<uint8_t>(ck >> 8);
        p[3] = static_cast<uint8_t>(ck & 0xff);
    }
    return p;
}

std::optional<ParsedReply> parse_v4(const uint8_t* buf, size_t len) {
    if (len < 20) return std::nullopt;
    size_t ihl = static_cast<size_t>(buf[0] & 0x0f) * 4;
    if (len < ihl + 8) return std::nullopt;
    const uint8_t* icmp = buf + ihl;
    size_t icmplen = len - ihl;
    uint8_t t = icmp[0];
    if (t == 0) { // echo reply
        return ParsedReply{ReplyKind::EchoReply, be16(icmp + 4), be16(icmp + 6), 1, {}};
    }
    if (t == 11 || t == 3) { // time exceeded / dest unreachable
        if (icmplen < 8) return std::nullopt;
        const uint8_t* inner = icmp + 8;
        size_t innerlen = icmplen - 8;
        if (innerlen < 20) return std::nullopt;
        size_t iihl = static_cast<size_t>(inner[0] & 0x0f) * 4;
        if (innerlen < iihl + 8) return std::nullopt;
        uint8_t orig_proto = inner[9]; // IPv4 header byte 9 = protocol (1=ICMP, 6=TCP, 17=UDP)
        const uint8_t* orig = inner + iihl;
        ReplyKind kind = (t == 11) ? ReplyKind::TimeExceeded : ReplyKind::Unreachable;
        auto mpls = parse_mpls_extensions(icmp, icmplen);
        if (orig_proto == 6 || orig_proto == 17) {
            // TCP/UDP embedded original — see ParsedReply::orig_proto's doc
            // comment (icmp.hpp) for why this reads different bytes than
            // the ICMP-embedded case below: bytes 0-1 are source port for
            // both; bytes 2-3 are dest port (UDP) or unused-for-matching
            // here (TCP); TCP additionally has a real sequence number at
            // bytes 4-7 where UDP has length+checksum instead — matches
            // probe_tcp.hpp's parse_tcp_in_icmp_v4 / probe_udp.hpp's
            // parse_udp_in_icmp_v4, unified here into the single dispatch
            // path every session's replies already flow through.
            uint16_t src_port = be16(orig);
            uint16_t second = (orig_proto == 6) ? be16(orig + 6) : be16(orig + 2);
            return ParsedReply{kind, src_port, second, orig_proto, mpls};
        }
        // ICMP-embedded original (orig_proto == 1, or anything else
        // unrecognized falls back to this — matches the pre-existing,
        // long-proven behavior exactly).
        return ParsedReply{kind, be16(orig + 4), be16(orig + 6), 1, mpls};
    }
    return std::nullopt;
}

std::optional<ParsedReply> parse_v6(const uint8_t* buf, size_t len) {
    if (len < 8) return std::nullopt;
    uint8_t t = buf[0];
    if (t == 129) { // echo reply
        return ParsedReply{ReplyKind::EchoReply, be16(buf + 4), be16(buf + 6), 1, {}};
    }
    if (t == 3 || t == 1) { // time exceeded / dest unreachable
        const uint8_t* inner = buf + 8;
        size_t innerlen = len - 8;
        if (innerlen < 48) return std::nullopt; // 40 (IPv6 hdr) + 8 (orig header's first 8 bytes)
        uint8_t orig_proto = inner[6]; // IPv6 header byte 6 = Next Header (same numbering as v4's protocol field)
        const uint8_t* orig = inner + 40;
        ReplyKind kind = (t == 3) ? ReplyKind::TimeExceeded : ReplyKind::Unreachable;
        auto mpls = parse_mpls_extensions(buf, len);
        if (orig_proto == 6 || orig_proto == 17) {
            uint16_t src_port = be16(orig);
            uint16_t second = (orig_proto == 6) ? be16(orig + 6) : be16(orig + 2);
            return ParsedReply{kind, src_port, second, orig_proto, mpls};
        }
        return ParsedReply{kind, be16(orig + 4), be16(orig + 6), 1, mpls};
    }
    return std::nullopt;
}

} // namespace netpulse

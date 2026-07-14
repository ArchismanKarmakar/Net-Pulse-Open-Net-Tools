#include "netpulse/icmp.hpp"

namespace netpulse {

static inline uint16_t be16(const uint8_t* b) {
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
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
        return ParsedReply{ReplyKind::EchoReply, be16(icmp + 4), be16(icmp + 6)};
    }
    if (t == 11 || t == 3) { // time exceeded / dest unreachable
        if (icmplen < 8) return std::nullopt;
        const uint8_t* inner = icmp + 8;
        size_t innerlen = icmplen - 8;
        if (innerlen < 20) return std::nullopt;
        size_t iihl = static_cast<size_t>(inner[0] & 0x0f) * 4;
        if (innerlen < iihl + 8) return std::nullopt;
        const uint8_t* orig = inner + iihl;
        return ParsedReply{t == 11 ? ReplyKind::TimeExceeded : ReplyKind::Unreachable,
                           be16(orig + 4), be16(orig + 6)};
    }
    return std::nullopt;
}

std::optional<ParsedReply> parse_v6(const uint8_t* buf, size_t len) {
    if (len < 8) return std::nullopt;
    uint8_t t = buf[0];
    if (t == 129) { // echo reply
        return ParsedReply{ReplyKind::EchoReply, be16(buf + 4), be16(buf + 6)};
    }
    if (t == 3 || t == 1) { // time exceeded / dest unreachable
        const uint8_t* inner = buf + 8;
        size_t innerlen = len - 8;
        if (innerlen < 48) return std::nullopt; // 40 (IPv6 hdr) + 8 (orig icmp)
        const uint8_t* orig = inner + 40;
        return ParsedReply{t == 3 ? ReplyKind::TimeExceeded : ReplyKind::Unreachable,
                           be16(orig + 4), be16(orig + 6)};
    }
    return std::nullopt;
}

} // namespace netpulse

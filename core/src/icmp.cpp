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

std::vector<uint8_t> build_echo(Family f, uint16_t id, uint16_t seq, size_t payload) {
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

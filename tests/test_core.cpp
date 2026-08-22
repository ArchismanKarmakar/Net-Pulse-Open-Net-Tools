// Minimal assertion-based tests for the NetPulse C++ core.
// Build: g++ -std=c++17 -I core/include core/src/*.cpp tests/test_core.cpp -o nptest -pthread
#include "netpulse/icmp.hpp"
#include "netpulse/stats.hpp"
#include "netpulse/session.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace netpulse;

static int g_failures = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__);         \
            ++g_failures;                                                   \
        } else {                                                            \
            std::printf("  ok: %s\n", #cond);                              \
        }                                                                   \
    } while (0)

static std::vector<uint8_t> ipv4_wrap(uint8_t proto, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> h(20, 0);
    h[0] = 0x45; // version 4, IHL 5
    h[9] = proto;
    h.insert(h.end(), payload.begin(), payload.end());
    return h;
}
static std::vector<uint8_t> ipv6_wrap(uint8_t next, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> h(40, 0);
    h[0] = 0x60;
    h[6] = next;
    h.insert(h.end(), payload.begin(), payload.end());
    return h;
}

int main() {
    std::printf("[icmp]\n");
    {
        auto pkt = build_echo(Family::V4, 0x1234, 7, 16);
        CHECK(checksum(pkt.data(), pkt.size()) == 0); // re-sum of valid checksum is 0
    }
    {
        // Paris-traceroute-style checksum pinning (Pillar 3's ECMP false-loop
        // fix): the whole point is that the FINAL checksum is identical across
        // different `seq` values for the same fixed pin target, so any router
        // hashing ECMP path selection on the ICMP checksum always picks the
        // same path for every probe in a session, regardless of hop/TTL.
        uint16_t target = 0xBEEF;
        auto a = build_echo(Family::V4, 0x1234, 1, 16, target);
        auto b = build_echo(Family::V4, 0x1234, 2, 16, target);
        auto c = build_echo(Family::V4, 0x1234, 9999, 16, target);
        CHECK(checksum(a.data(), a.size()) == 0); // still a valid, self-consistent checksum
        CHECK(checksum(b.data(), b.size()) == 0);
        CHECK(checksum(c.data(), c.size()) == 0);
        uint16_t ck_a = static_cast<uint16_t>((a[2] << 8) | a[3]);
        uint16_t ck_b = static_cast<uint16_t>((b[2] << 8) | b[3]);
        uint16_t ck_c = static_cast<uint16_t>((c[2] << 8) | c[3]);
        CHECK(ck_a == target); // the actual property: checksum field == the pin, exactly
        CHECK(ck_b == target);
        CHECK(ck_c == target);
        // Too-small payload: pinning is silently skipped, packet still valid.
        auto small = build_echo(Family::V4, 0x1234, 5, 1, target);
        CHECK(checksum(small.data(), small.size()) == 0);
    }
    {
        auto echo = build_echo(Family::V4, 0xBEEF, 42, 8);
        echo[0] = 0; // echo reply
        auto frame = ipv4_wrap(1, echo);
        auto r = parse_v4(frame.data(), frame.size());
        CHECK(r.has_value());
        CHECK(r->kind == ReplyKind::EchoReply);
        CHECK(r->id == 0xBEEF);
        CHECK(r->seq == 42);
    }
    {
        auto orig = build_echo(Family::V4, 0xABCD, 9, 8);
        auto inner_ip = ipv4_wrap(1, orig);
        std::vector<uint8_t> err = {11, 0, 0, 0, 0, 0, 0, 0};
        err.insert(err.end(), inner_ip.begin(), inner_ip.end());
        auto frame = ipv4_wrap(1, err);
        auto r = parse_v4(frame.data(), frame.size());
        CHECK(r.has_value());
        CHECK(r->kind == ReplyKind::TimeExceeded);
        CHECK(r->id == 0xABCD);
        CHECK(r->seq == 9);
    }
    {
        auto echo = build_echo(Family::V6, 0x0F0F, 3, 8);
        echo[0] = 129; // echo reply
        auto r = parse_v6(echo.data(), echo.size());
        CHECK(r.has_value());
        CHECK(r->kind == ReplyKind::EchoReply);
        CHECK(r->id == 0x0F0F);
        CHECK(r->seq == 3);
    }
    {
        auto orig = build_echo(Family::V6, 0x7777, 5, 8);
        auto inner_ip = ipv6_wrap(58, orig);
        std::vector<uint8_t> err = {3, 0, 0, 0, 0, 0, 0, 0};
        err.insert(err.end(), inner_ip.begin(), inner_ip.end());
        auto r = parse_v6(err.data(), err.size());
        CHECK(r.has_value());
        CHECK(r->kind == ReplyKind::TimeExceeded);
        CHECK(r->id == 0x7777);
        CHECK(r->seq == 5);
    }
    {
        // RFC 4884/4950: Time Exceeded carrying an MPLS label stack extension.
        auto orig = build_echo(Family::V4, 0xABCD, 9, 8);
        auto inner_ip = ipv4_wrap(1, orig); // 36 bytes -> 9 words
        std::vector<uint8_t> ext = {0x20, 0x00, 0x00, 0x00,  // common header (checksum filled below)
                                     0x00, 0x08, 1,    1,     // object header: length=8, class=1 (MPLS), c-type=1
                                     0,    0,    0,    0};    // label entry, filled below
        uint32_t label_val = (16003u << 12) | (0u << 9) | (1u << 8) | 1u; // label=16003 exp=0 S=1 ttl=1
        ext[8] = static_cast<uint8_t>(label_val >> 24);
        ext[9] = static_cast<uint8_t>(label_val >> 16);
        ext[10] = static_cast<uint8_t>(label_val >> 8);
        ext[11] = static_cast<uint8_t>(label_val);
        uint16_t ck = checksum(ext.data(), ext.size());
        ext[2] = static_cast<uint8_t>(ck >> 8);
        ext[3] = static_cast<uint8_t>(ck & 0xff);
        CHECK(checksum(ext.data(), ext.size()) == 0);

        std::vector<uint8_t> err = {11, 0, 0, 0, 0, static_cast<uint8_t>(inner_ip.size() / 4), 0, 0};
        err.insert(err.end(), inner_ip.begin(), inner_ip.end());
        err.insert(err.end(), ext.begin(), ext.end());
        auto frame = ipv4_wrap(1, err);
        auto r = parse_v4(frame.data(), frame.size());
        CHECK(r.has_value());
        CHECK(r->kind == ReplyKind::TimeExceeded);
        CHECK(r->mpls_labels.size() == 1);
        if (!r->mpls_labels.empty()) {
            uint32_t v = r->mpls_labels[0];
            CHECK((v >> 12) == 16003);
            CHECK(((v >> 9) & 0x7) == 0);
            CHECK(((v >> 8) & 0x1) == 1);
            CHECK((v & 0xff) == 1);
        }
    }
    {
        // Legacy router: RFC 4884 length hint left at 0 -> no extension parsing attempted.
        auto orig = build_echo(Family::V4, 0xABCD, 9, 8);
        auto inner_ip = ipv4_wrap(1, orig);
        std::vector<uint8_t> err = {11, 0, 0, 0, 0, 0, 0, 0}; // byte 5 == 0 => no RFC 4884 hint
        err.insert(err.end(), inner_ip.begin(), inner_ip.end());
        auto frame = ipv4_wrap(1, err);
        auto r = parse_v4(frame.data(), frame.size());
        CHECK(r.has_value());
        CHECK(r->mpls_labels.empty());
    }

    std::printf("[stats]\n");
    {
        HopStats h(1, 1);
        double base = now_secs();
        for (int i = 0; i < 10; ++i) {
            std::optional<double> rtt = (i == 2) ? std::nullopt : std::optional<double>(i);
            h.push(base - (9 - i), rtt);
        }
        auto all = h.compute(std::nullopt);
        CHECK(all.sent == 10);
        CHECK(all.recv == 9);
        CHECK(all.max.has_value() && *all.max == 9.0);
        auto win = h.compute(3.5);
        CHECK(win.sent == 4);
        CHECK(win.min.has_value() && *win.min == 6.0);
    }
    {
        HopStats h(1, 2);
        h.set_address("10.0.0.1");
        h.push(now_secs(), 5.0);
        h.set_address("10.0.0.2"); // route flap
        auto s = h.compute(std::nullopt);
        CHECK(s.sent == 0); // history cleared
    }

    std::printf("[loop auditor: compute_max_hop]\n");
    {
        // No destination yet, no loop: normal sliding window (frontier + kDiscoveryWindow), capped at max_hops.
        CHECK(compute_max_hop(std::nullopt, std::nullopt, 5, 30) == 5 + kDiscoveryWindow);
        CHECK(compute_max_hop(std::nullopt, std::nullopt, 28, 30) == 30); // capped at max_hops
        CHECK(compute_max_hop(std::nullopt, std::nullopt, 0, 30) == kDiscoveryWindow); // floor: frontier=0 still yields >=1
    }
    {
        // Destination known: exactly that, regardless of frontier or any loop state.
        CHECK(compute_max_hop(uint8_t(12), std::nullopt, 20, 30) == 12);
        CHECK(compute_max_hop(uint8_t(12), uint8_t(3), 20, 30) == 12); // dest_hop wins even if a loop was flagged earlier
    }
    {
        // Confirmed loop, no destination: window FREEZES at loop_at_hop + kLoopAuditWindow,
        // ignoring frontier entirely — this is the actual fix for the "ghost train"
        // (ordinarily frontier would keep sliding the window out to max_hops).
        CHECK(compute_max_hop(std::nullopt, uint8_t(5), 25, 30) == 5 + kLoopAuditWindow);
        CHECK(compute_max_hop(std::nullopt, uint8_t(5), 25, 30) != 25 + kDiscoveryWindow);
        // Still respects the configured ceiling.
        CHECK(compute_max_hop(std::nullopt, uint8_t(29), 25, 30) == 30);
    }

    std::printf("[udp_give_up_threshold]\n");
    {
        // Default-ish config (probe=1s, max_hops=30): 3 sweeps = 90s, well
        // above the 30s floor, so the sweep math wins.
        CHECK(udp_give_up_threshold(1.0, 30) == 90.0);
        // Fast probing / short hop range: 3 sweeps would be tiny (e.g.
        // 0.5s * 5 * 3 = 7.5s) — the floor protects against declaring
        // "likely blocked" absurdly early.
        CHECK(udp_give_up_threshold(0.5, 5) == kUdpGiveUpMinSecs);
        // A long, slow path (probe=2s, max_hops=64) legitimately needs
        // longer than the floor — 2*64*3 = 384s.
        CHECK(udp_give_up_threshold(2.0, 64) == 384.0);
        // Exactly at the floor boundary: 30s*max_hops... pick inputs that
        // land exactly on kUdpGiveUpMinSecs to confirm the max() doesn't
        // round or drop below it.
        CHECK(udp_give_up_threshold(1.0, 10) == kUdpGiveUpMinSecs); // 1*10*3=30 == floor
    }

    std::printf("[silence_backoff_threshold]\n");
    {
        // No prior silence-triggered rebuilds: threshold is unchanged.
        CHECK(silence_backoff_threshold(15.0, 0) == 15.0);
        CHECK(silence_backoff_threshold(15.0, -1) == 15.0); // defensive: never negative-index the backoff
        // Grows by kSilenceBackoffMult per consecutive rebuild...
        CHECK(silence_backoff_threshold(15.0, 1) == 15.0 * kSilenceBackoffMult);
        CHECK(silence_backoff_threshold(15.0, 2) == 15.0 * kSilenceBackoffMult * kSilenceBackoffMult);
        // ...but never exceeds the cap, however many consecutive rebuilds
        // there have been (a target stuck for weeks shouldn't wait longer
        // than kSilenceBackoffCapSecs between retries).
        CHECK(silence_backoff_threshold(15.0, 50) == kSilenceBackoffCapSecs);
        CHECK(silence_backoff_threshold(60.0, 50) == kSilenceBackoffCapSecs);
        // The exponent-growth cap (kSilenceBackoffCapAt) shouldn't matter in
        // practice — kSilenceBackoffCapSecs is reached well before it — but
        // confirm going past it doesn't overflow/misbehave either.
        CHECK(silence_backoff_threshold(15.0, kSilenceBackoffCapAt + 5) == kSilenceBackoffCapSecs);
    }

    std::printf("[public IP classification]\n");
    {
        CHECK(is_public_ip("1.1.1.1"));
        CHECK(is_public_ip("117.194.112.1")); // the original BSNL BNG from the bug report
        CHECK(!is_public_ip("192.168.1.1"));
        CHECK(!is_public_ip("10.1.5.12"));
        CHECK(!is_public_ip("172.16.0.1"));
        CHECK(!is_public_ip("100.64.0.1")); // CGNAT
        CHECK(!is_public_ip("127.0.0.1"));
        CHECK(!is_public_ip(""));
        CHECK(!is_public_ip("*"));
        CHECK(is_public_ip("2606:4700:4700::1111")); // 1.1.1.1's AAAA
        CHECK(!is_public_ip("fe80::1"));
        CHECK(!is_public_ip("fd00::1")); // ULA
        CHECK(!is_public_ip("::1"));
    }

    std::printf("[SharedHopTable: edge-attributed key + public gate]\n");
    {
        // Same (source, predecessor, responder) edge: a DIFFERENT owner adopts a fresh sample.
        SharedHopTable sh;
        double t0 = 1000.0;
        shared_publish_to(sh, "10.0.0.5", "10.0.0.1", "117.194.112.1", t0, 4.2, /*owner*/ 1, /*hop*/ 2);
        auto v = shared_adopt_from(sh, "10.0.0.5", "10.0.0.1", "117.194.112.1", /*self*/ 2, t0 + 0.1, /*max_age*/ 1.0);
        CHECK(v.has_value() && *v == 4.2);
    }
    {
        // Same owner never adopts its own entry (would defeat "the owner always probes for real").
        SharedHopTable sh;
        shared_publish_to(sh, "10.0.0.5", "10.0.0.1", "117.194.112.1", 1000.0, 4.2, /*owner*/ 7, 2);
        auto v = shared_adopt_from(sh, "10.0.0.5", "10.0.0.1", "117.194.112.1", /*self*/ 7, 1000.1, 1.0);
        CHECK(!v.has_value());
    }
    {
        // Stale entry (older than max_age) is not adopted — self-healing, not sticky.
        SharedHopTable sh;
        shared_publish_to(sh, "10.0.0.5", "10.0.0.1", "117.194.112.1", 1000.0, 4.2, 1, 2);
        auto v = shared_adopt_from(sh, "10.0.0.5", "10.0.0.1", "117.194.112.1", 2, 1002.0, 1.0);
        CHECK(!v.has_value());
    }
    {
        // Same responder IP, DIFFERENT predecessor: this is the user's exact
        // "10.1.5.12 at two different hop depths, two different real devices"
        // scenario — must NOT cross-adopt, or their real numbers would overwrite
        // each other.
        SharedHopTable sh;
        shared_publish_to(sh, "10.0.0.5", "203.0.113.9", "10.1.5.12", 1000.0, 5.0, 1, 5);   // target A's edge
        shared_publish_to(sh, "10.0.0.5", "198.51.100.4", "10.1.5.12", 1000.0, 80.0, 2, 8); // target B's DIFFERENT edge, same responder
        // But wait — 10.1.5.12 is private, so neither publish should even land (gate below).
        // Re-run with a PUBLIC responder to isolate the predecessor-differentiation behavior:
        SharedHopTable sh2;
        shared_publish_to(sh2, "10.0.0.5", "203.0.113.9", "117.216.207.208", 1000.0, 5.0, 1, 5);
        shared_publish_to(sh2, "10.0.0.5", "198.51.100.4", "117.216.207.208", 1000.0, 80.0, 2, 8);
        auto via_a = shared_adopt_from(sh2, "10.0.0.5", "203.0.113.9", "117.216.207.208", 99, 1000.1, 1.0);
        auto via_b = shared_adopt_from(sh2, "10.0.0.5", "198.51.100.4", "117.216.207.208", 99, 1000.1, 1.0);
        CHECK(via_a.has_value() && *via_a == 5.0);
        CHECK(via_b.has_value() && *via_b == 80.0); // independent entry, NOT overwritten by the other edge's sample
        // And the private-IP publishes from the first block truly never landed:
        CHECK(sh.map.empty());
    }
    {
        // Same edge, SAME predecessor (including both "SRC"): this is the
        // common, valuable, safe-sharing case and must still work.
        SharedHopTable sh;
        shared_publish_to(sh, "10.0.0.5", "SRC", "1.1.1.1", 1000.0, 12.0, 1, 1);
        auto v = shared_adopt_from(sh, "10.0.0.5", "SRC", "1.1.1.1", 2, 1000.1, 1.0);
        CHECK(v.has_value() && *v == 12.0);
    }
    {
        // Public-IP gate: a private responder is never published, so no other
        // session can ever adopt a sample for it, regardless of predecessor.
        SharedHopTable sh;
        shared_publish_to(sh, "10.0.0.5", "SRC", "192.168.1.1", 1000.0, 1.0, 1, 1);
        CHECK(sh.map.empty());
        auto v = shared_adopt_from(sh, "10.0.0.5", "SRC", "192.168.1.1", 2, 1000.1, 1.0);
        CHECK(!v.has_value());
    }

    std::printf("[session resolve]\n");
    {
        Settings st;
        st.family = FamilyPref::V4;
        Session s(1, "127.0.0.1", st);
        s.resolve();
        CHECK(s.dest().has_value() && *s.dest() == "127.0.0.1");
        CHECK(s.fam().has_value() && *s.fam() == Family::V4);
    }
    {
        Settings st;
        st.family = FamilyPref::V6;
        Session s(2, "::1", st);
        s.resolve();
        CHECK(s.dest().has_value() && *s.dest() == "::1");
        CHECK(s.fam().has_value() && *s.fam() == Family::V6);
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}

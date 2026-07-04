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

    std::printf("[stats]\n");
    {
        HopStats h(1);
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
        HopStats h(2);
        h.set_address("10.0.0.1");
        h.push(now_secs(), 5.0);
        h.set_address("10.0.0.2"); // route flap
        auto s = h.compute(std::nullopt);
        CHECK(s.sent == 0); // history cleared
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

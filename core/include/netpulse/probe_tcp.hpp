// TCP ProbeStrategy — structurally different from ICMP's, not a variant of
// it, for one unavoidable reason: Windows has refused to send a manually
// crafted TCP SYN through a raw socket since XP SP2 (a deliberate,
// permanent anti-spoofing restriction, not a privilege you can escalate
// past). Every real Windows TCP-ping/traceroute tool (tracetcp, tcping,
// PowerShell's Test-NetConnection) works around this the same way: let the
// OS's own TCP stack build and send the SYN, via an ordinary (non-raw),
// non-blocking connect() with IP_TTL/IPV6_UNICAST_HOPS set before
// connecting, and read the outcome back through the normal socket API
// (connect completes = SYN-ACK; ECONNREFUSED/WSAECONNREFUSED = RST) rather
// than parsing raw wire bytes. So unlike probe_icmp.hpp, this class can't
// share Prober's send()/one-pooled-socket-per-family model at all — it
// owns a small pool of short-lived per-probe sockets instead, each
// tracked from send until it either completes or times out.
//
// What TCP probing does NOT need new code for: intermediate-hop replies.
// A router that TTL-expires a packet generates an ICMP Time-Exceeded
// regardless of whether the original packet was ICMP, TCP, or UDP — an
// IP-layer behavior, not transport-layer (see probe.hpp's ProbeReply doc
// comment). So a TTL-limited SYN's hop-discovery reply still arrives on
// the SAME shared ICMP raw socket / RX dispatcher that ICMP mode already
// uses, completely unmodified — this class only needs to teach that path
// how to recognize a TCP segment embedded in a Time-Exceeded/Unreachable
// message (parse_tcp_in_icmp_v4/v6 below), since the embedded header
// layout is genuinely different (source/dest port + sequence number, not
// an ICMP id/seq pair at the same byte offsets icmp.cpp's parsers expect).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "netpulse/icmp.hpp" // Family, ReplyKind, ParsedReply — same taxonomy, new producer
#include "netpulse/probe.hpp"

namespace netpulse {

// Same three-field shape as ParsedReply (icmp.hpp), but sourced from an
// embedded TCP header instead of an embedded ICMP one — `id`/`seq` here
// are DIFFERENT bit patterns semantically (see parse_tcp_in_icmp_v4's doc
// comment: id doubles as the source port, seq as the low 16 bits of the
// TCP sequence number), but same shape so this plugs into the existing
// id/seq-keyed matching Session already does for ICMP, unmodified.
std::optional<ParsedReply> parse_tcp_in_icmp_v4(const uint8_t* buf, size_t len);
std::optional<ParsedReply> parse_tcp_in_icmp_v6(const uint8_t* buf, size_t len);

class ProbeTcp : public ProbeStrategy {
public:
    // `dest_port` is the TCP port every probe (hop-discovery and direct)
    // connects to — user-configurable (default 80/443, same UX as any
    // real TCP-ping tool asking "which port"). `family`/`source` mirror
    // Prober's constructor (transport.hpp) — bind to a specific local
    // address/interface the same way ICMP mode already supports.
    ProbeTcp(Family family, uint16_t dest_port, std::string source = std::string());
    ~ProbeTcp() override;
    ProbeTcp(const ProbeTcp&) = delete;
    ProbeTcp& operator=(const ProbeTcp&) = delete;

    const char* name() const override { return "tcp"; }

    bool send_hop_probe(const std::string& dest_ip, uint8_t ttl, uint16_t seq,
                         size_t payload, uint32_t pin) override;
    bool send_direct_probe(const std::string& dest_ip, uint16_t seq,
                            size_t payload, uint32_t pin) override;

    // True while any probe is still awaiting a connect() completion. Exists
    // so a caller's event loop can tell the difference between "idle, safe
    // to sleep a while" and "a SYN is in flight, poll_completions() needs to
    // run SOON or the measured RTT is just however long we slept" — see
    // run_tcp()'s own wait-duration comment (session.cpp) for the bug this
    // was added to fix.
    bool has_pending() const;

    // The local port the most recent successfully-opened probe is
    // ACTUALLY bound to, confirmed via getsockname() rather than assumed
    // from (pin + ttl) — see open_probe()'s bind comment (probe_tcp.cpp)
    // for why assuming it silently breaks hop correlation. Callers that
    // register a reply key must use THIS, not their own arithmetic.
    // Only meaningful immediately after a send_*_probe() that returned
    // true; atomic because poll_completions() may run concurrently on
    // another thread, though only the sending thread reads this.
    uint16_t last_bound_port() const { return last_bound_port_.load(); }

    // A source port to bind every probe socket to — see this file's top
    // comment: TCP/UDP's port is STRICTLY BETTER than ICMP's
    // checksum-pinning hack, because it's not a workaround at all, it's
    // the literal field real ECMP implementations hash on. Ports below
    // 1024 need elevated privileges on most OSes to bind explicitly, so
    // this picks from the ephemeral range (49152-65535) — plenty of
    // spread for pinning purposes, no privilege requirement.
    uint32_t new_flow_pin() override;

    // Non-blocking: check every outstanding probe socket for completion
    // (connected = destination answered; refused = destination answered
    // with RST; still nothing after this session's own local budget =
    // give up on THIS socket, the real signal — if any — will have
    // already arrived via the shared ICMP path for a hop-probe, or this
    // was simple loss for a direct probe) and return finished ones.
    // Called the same way Prober::drain_ready() is — from a poll loop,
    // not per-probe — see this file's top comment for why TCP can't reuse
    // drain_ready() itself (no single shared socket to read from).
    std::vector<ProbeReply> poll_completions(double now, double timeout_secs);

private:
    struct Pending {
        long long fd;
        std::string dest_ip;
        uint16_t seq;
        double sent_at;
        // BUG FIX: RTT is a DURATION, so it must be measured on a
        // monotonic clock. sent_at above is wall-clock (system_clock) and
        // is still what gets reported as the sample TIMESTAMP — the UI's
        // chart axis needs real calendar time, so that cannot change. But
        // computing (now - sent_at) on the wall clock means any step in it
        // lands directly in the measurement: an NTP correction or a
        // sleep/wake jump mid-flight produces a wildly wrong or even
        // negative RTT for every probe outstanding at that moment.
        // sent_at_mono is immune to both; use it for the RTT and for the
        // timeout comparison, and keep sent_at purely for the timestamp.
        double sent_at_mono = 0.0;
        bool is_direct; // ProbeOutcome::DirectReply either way once THIS socket completes —
                        // a hop-discovery probe that itself gets a SYN-ACK/RST just means the
                        // real path was shorter than this TTL, which is valid, useful information,
                        // not a hop-vs-direct ambiguity to resolve here.
    };

    bool open_probe(const std::string& dest_ip, uint8_t ttl, uint16_t seq, uint32_t pin, bool is_direct);

    Family family_;
    uint16_t dest_port_;
    std::string source_;
    std::atomic<uint16_t> last_bound_port_{0}; // see last_bound_port() above
    mutable std::mutex mtx_; // guards pending_ (mutable: has_pending() above is const but must still lock) — poll_completions() and open_probe() can race across threads
    std::map<long long, Pending> pending_; // keyed by fd
};

} // namespace netpulse

// HTTP ProbeStrategy — plain HTTP only (port 80 by default), deliberately
// no TLS. See probe_http.cpp's top comment for exactly why HTTPS isn't
// attempted here: it needs a real TLS library, not an extension of this.
//
// Hop-discovery is IDENTICAL to TCP mode — an intermediate router doesn't
// run an HTTP server, so there's nothing HTTP-specific to do for TTL-
// limited probes at all. This class holds a ProbeTcp internally and
// delegates send_hop_probe()/new_flow_pin() straight to it, unmodified.
//
// The one genuinely new piece: the direct probe. TCP mode's direct probe
// stops at a completed connect() — proof the destination is reachable at
// the transport layer. That's not the same claim as "this destination is
// actually running an HTTP server" (many things listen on a TCP port
// without speaking HTTP), so this class continues past the handshake:
// send a real HTTP request, then wait for any response bytes back before
// declaring success. That's a genuinely different, stronger check than
// TCP mode gives you, not a relabeling of it.
#pragma once
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>

#include "netpulse/probe.hpp"
#include "netpulse/probe_tcp.hpp"

namespace netpulse {

class ProbeHttp : public ProbeStrategy {
public:
    // `dest_port` defaults to 80 (plain HTTP's standard port). `host` is
    // the value sent in the request's Host: header — should be the
    // target's ORIGINAL name (e.g. "example.com"), not its resolved IP,
    // so virtual-hosted servers (the overwhelming majority of real HTTP
    // deployments — many sites on one IP) respond meaningfully instead of
    // with a generic/default vhost's response. Falls back to the IP
    // itself if no better name is available — still often gets a real
    // response (even an error page proves the server is there and
    // speaking HTTP), just not necessarily from the intended site.
    ProbeHttp(Family family, uint16_t dest_port, std::string host, std::string source = std::string());
    ~ProbeHttp() override;
    ProbeHttp(const ProbeHttp&) = delete;
    ProbeHttp& operator=(const ProbeHttp&) = delete;

    const char* name() const override { return "http"; }

    // Hop-discovery: pure delegation to the internal ProbeTcp — see this
    // file's top comment for why there's nothing HTTP-specific to add
    // here at all.
    bool send_hop_probe(const std::string& dest_ip, uint8_t ttl, uint16_t seq,
                         size_t payload, uint32_t pin) override;

    // NOT a delegation — opens its own dedicated connection and tracks it
    // through to an actual HTTP response, not just a completed handshake.
    bool send_direct_probe(const std::string& dest_ip, uint16_t seq,
                            size_t payload, uint32_t pin) override;

    uint32_t new_flow_pin() override;

    // Same calling convention as ProbeTcp::poll_completions — called from
    // a poll loop, checks every outstanding HTTP probe for progress
    // (connected but not yet requested → send the request; requested but
    // not yet answered → check for response bytes) and returns ones that
    // completed (with an actual HTTP-level response, not just a TCP
    // handshake) or definitively failed.
    std::vector<ProbeReply> poll_completions(double now, double timeout_secs);

    // True while any probe is still awaiting completion. Same purpose as
    // ProbeTcp::has_pending() — see run_http()'s wait-duration comment
    // (session.cpp) for the RTT-quantization bug this exists to fix.
    bool has_pending() const;

    // The local port the most recent successfully-opened probe is ACTUALLY
    // bound to, confirmed via getsockname(). Identical purpose and identical
    // bug history to ProbeTcp::last_bound_port() — see that accessor's doc
    // comment. Callers registering a reply key must use THIS, never their
    // own (pin + ttl) arithmetic.
    uint16_t last_bound_port() const { return last_bound_port_.load(); }

private:
    struct Pending {
        long long fd;
        std::string dest_ip;
        uint16_t seq;
        double sent_at;      // when this probe was opened — RTT is measured end-to-end (connect + request + response), not just connect time
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
        bool connected = false;
        bool request_sent = false;
    };

    Family family_;
    uint16_t dest_port_;
    std::string host_;
    std::string source_;
    std::unique_ptr<ProbeTcp> hop_prober_; // hop-discovery only — see this file's top comment
    std::atomic<uint16_t> last_bound_port_{0}; // see last_bound_port() above
    mutable std::mutex mtx_; // mutable: has_pending() is const but must still lock
    std::map<long long, Pending> pending_;
};

} // namespace netpulse

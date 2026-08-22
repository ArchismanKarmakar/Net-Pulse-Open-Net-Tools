// ICMP ProbeStrategy — a thin adapter, not a reimplementation. Every call
// here forwards directly to the existing Prober::send() (transport.hpp),
// completely unmodified. This file's only job is exposing that existing,
// already-verified code through the new ProbeStrategy seam (probe.hpp) so
// Session can eventually be written against the interface instead of
// naming Prober directly — see probe.hpp's top comment for the full
// rationale.
//
// NOT covered by this class, deliberately: ICMP reply ROUTING (matching an
// incoming reply's id field back to the owning Session) is a separate,
// pre-existing mechanism (IcmpOwner/g_registry in session.hpp/session.cpp)
// that has nothing to do with SENDING probes — it stays exactly as it is,
// entirely inside Session, untouched by this refactor. This class only
// owns the send side, plus generating the fixed-checksum ECMP-pinning
// token, which IS protocol-generic (every ProbeStrategy needs some
// equivalent — see new_flow_pin()'s doc comment in probe.hpp).
#pragma once
#include <memory>

#include "netpulse/probe.hpp"
#include "netpulse/transport.hpp"

namespace netpulse {

class ProbeIcmp : public ProbeStrategy {
public:
    // `prober` is a pooled socket acquired exactly as session.cpp does
    // today (acquire_pooled_socket) — this class takes shared ownership,
    // same lifetime semantics as the existing direct Prober usage.
    // `icmp_id` is the session's existing icmp_id_ (assignment/collision
    // avoidance against g_registry is unchanged, still Session's job — see
    // this file's top comment).
    ProbeIcmp(std::shared_ptr<Prober> prober, uint16_t icmp_id)
        : prober_(std::move(prober)), icmp_id_(icmp_id) {}

    const char* name() const override { return "icmp"; }

    bool send_hop_probe(const std::string& dest_ip, uint8_t ttl, uint16_t seq,
                         size_t payload, uint32_t pin) override {
        return prober_->send(dest_ip, ttl, icmp_id_, seq, payload, pin_to_checksum(pin));
    }

    bool send_direct_probe(const std::string& dest_ip, uint16_t seq,
                            size_t payload, uint32_t pin) override {
        return prober_->send(dest_ip, kDirectEchoTtl, icmp_id_, seq, payload, pin_to_checksum(pin));
    }

    // ICMP has no source port to pin on, hence the existing checksum trick
    // (see build_echo's pin_checksum doc comment in icmp.hpp) — this just
    // needs to be SOME fixed 16-bit value, stable for this session's
    // lifetime, distinct enough across sessions that two different targets
    // don't coincidentally pin themselves onto the exact same value (which
    // would be harmless — Gate 2's edge-key already disambiguates by
    // predecessor+ip too — but distinct is still preferable). Reusing
    // icmp_id_ itself is a convenient, already-unique-per-session source;
    // XORing with a session-local counter-ish constant just avoids the
    // checksum and the id colliding bit-for-bit for no reason.
    //
    // NOT used by the real session.cpp integration, deliberately — see
    // pin_to_checksum()'s doc comment for why.
    uint32_t new_flow_pin() override { return static_cast<uint32_t>(icmp_id_ ^ 0xA5A5u) & 0xFFFFu; }

private:
    // Maps the generic pin token down to icmp.hpp's pin_checksum parameter
    // type. Deliberately does NOT treat pin==0 as "no pinning requested"
    // the way probe_tcp.hpp/probe_udp.hpp's port-pinning does — found by
    // actually checking session.cpp's real paris_checksum_target_ before
    // wiring this in: its derivation explicitly remaps 0xFFFF to 0x0000
    // (see its comment in session.cpp), so 0 is a legitimate, real pin
    // value for some sessions, not a sentinel. Every existing ICMP call
    // site always wants pinning (there's no call site that passes "don't
    // pin" today), so this always treats `pin` as a literal checksum
    // target — never nullopt. A TCP/UDP-style "0 means let the OS choose"
    // convention would have been a real, silent correctness bug here:
    // exactly the sessions whose paris_checksum_target_ happened to
    // compute to 0 would have silently stopped being ECMP-pinned at all.
    static std::optional<uint16_t> pin_to_checksum(uint32_t pin) {
        return static_cast<uint16_t>(pin & 0xFFFFu);
    }

    std::shared_ptr<Prober> prober_;
    uint16_t icmp_id_;

    // Mirrors session.cpp's own kDirectEchoTtl (max TTL for a direct/echo
    // probe — maximum survivability, always valid regardless of real path
    // length). Duplicated here rather than shared because it's a tiny,
    // ICMP-specific constant that has no reason to become part of the
    // generic ProbeStrategy interface — a future TCP/UDP direct probe
    // doesn't need a "TTL" concept the same way (it connects to the
    // destination's actual port, not a synthetic max-TTL echo).
    static constexpr uint8_t kDirectEchoTtl = 255;
};

} // namespace netpulse

// The protocol abstraction boundary — see ARCHITECTURE.md's addendum on
// multi-protocol probing for the full design rationale. This file exists to
// answer one question precisely: which of Session's responsibilities are
// ABOUT MEASUREMENT (protocol-specific: how do you send a probe, how do you
// recognize its reply) versus ABOUT THE SESSION'S OWN SAFETY GUARANTEES
// (protocol-agnostic: guarded-wipe thresholds, the loop auditor, shared-hop
// adoption, rate governance, self-healing rebuilds)? Everything in the
// second category — every "pillar" described in ARCHITECTURE.md — is
// implemented once, in Session, and stays completely unmodified regardless
// of which ProbeStrategy a session is using. Only the first category is
// what a new protocol actually needs to implement.
//
// This interface was extracted FROM Session's existing ICMP-specific code,
// not designed abstractly first — every method here corresponds to an
// operation Session already performs against Prober/icmp.hpp today. The
// ICMP implementation (probe_icmp.hpp) is a thin adapter around that
// existing, unmodified code: this refactor changes nothing about ICMP's
// behavior, it just gives Session a seam to call through instead of naming
// Prober directly, so a second implementation (TCP, first — see
// probe_tcp.hpp) can be handed to the exact same Session machinery.
#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace netpulse {

// Forward declaration only — defined in session.cpp, alongside the shared
// ICMP-owner registry it queries. Deliberately NOT `#include
// "netpulse/session.hpp"` here: that header already includes probe_tcp.hpp/
// probe_udp.hpp, which transitively include this file, so including it back
// would be a circular include; a bare declaration is all allocate_flow_
// port_block() below actually needs.
bool icmp_owner_port_in_use(uint16_t base_port, int span);

// A process-wide, collision-free source of distinct port ranges for any
// ProbeStrategy that needs one (ProbeTcp, ProbeUdp — see each one's
// new_flow_pin()). Deliberately NOT derived from `this`'s address the way
// an earlier version of this code did: that has zero collision avoidance
// across DIFFERENT sessions, of the SAME or DIFFERENT protocols, running
// simultaneously — and heap allocations for same-sized small objects
// (exactly what multiple Session/ProbeTcp/ProbeUdp instances are) often
// land in predictable, closely-clustered address ranges, especially when
// several targets get added in quick succession, which is the ordinary
// case, not a rare one. A genuine collision here is NOT the same kind of
// "harmless, Gate-2-disambiguated" coincidence the shared-hop ADOPTION
// cache tolerates (ARCHITECTURE.md §4) — this is the reply-ROUTING
// registry (register_icmp_owner, session.cpp), a plain map with no
// collision tolerance at all: whichever session registers a given port
// LAST simply wins, and the other session's replies silently vanish,
// with no error, indefinitely, exactly matching a report of "UDP mode
// permanently stuck, only when other protocol sessions are also active."
//
// BUG FIX: an earlier version of this doc comment (and the code below)
// claimed the wraparound case only risks a collision "after 64 SIMULTANEOUS
// sessions are already running, not on the first handful" — that reasoning
// was wrong. `counter` is a process-wide value that only ever increases; it
// has no concept of a slot being freed when a session stops, so what
// actually matters is 64 CUMULATIVE calls across the process's whole
// lifetime, not 64 concurrent ones. A long-running session with many
// targets added and removed over time — every MTR target and every Ping
// run each consume one call — reaches 64 total calls easily, and once the
// counter wraps, a brand-new session can be handed the exact same block as
// a still-running one. Live evidence matched this precisely: UDP ping
// working correctly for one target and then failing completely (100%
// loss, not partial/gradual) for others tested moments later — an
// either-fully-works-or-fully-doesn't pattern unrelated to the actual
// target is the signature of a silent registry collision, not a real
// per-destination network issue.
//
// Fixed by actually checking occupancy (icmp_owner_port_in_use(),
// session.cpp) before committing to a candidate block, rather than trusting
// the counter alone to avoid collisions it has no way to know about.
inline uint16_t allocate_flow_port_block() {
    static std::atomic<uint32_t> counter{0};
    uint32_t start_slot = counter.fetch_add(1, std::memory_order_relaxed) % 64u;
    for (uint32_t tries = 0; tries < 64u; ++tries) {
        uint32_t slot = (start_slot + tries) % 64u;
        uint16_t candidate = static_cast<uint16_t>(49152u + slot * 256u);
        if (!icmp_owner_port_in_use(candidate, 256)) return candidate;
    }
    // Every one of the 64 blocks genuinely occupied — legitimately rare
    // (dozens of simultaneous multi-hop sessions); degrade to the original
    // round-robin choice rather than fail the caller outright.
    return static_cast<uint16_t>(49152u + start_slot * 256u);
}

} // namespace netpulse

#include "netpulse/icmp.hpp" // Family, ReplyKind — reused: every protocol still ultimately gets
                             // its intermediate-hop signal from ICMP Time-Exceeded/Unreachable,
                             // see ProbeReply's doc comment below.

namespace netpulse {

// What a probe attempt resolved to, protocol-agnostic. Every protocol's
// "reply" — an ICMP Echo Reply, a TCP SYN-ACK/RST, a UDP Port-Unreachable,
// an HTTP response, a QUIC PING ack — collapses to one of these three
// outcomes from Session's point of view; the pillar machinery (guarded
// wipe, direct-vs-legacy state, loop auditor) only ever needs to know
// WHICH of these three happened and from where, never the protocol-level
// detail underneath.
enum class ProbeOutcome {
    // The destination itself answered — direct evidence this responder is
    // still there and reachable, regardless of hop count. ICMP: Echo Reply.
    // TCP: SYN-ACK or RST from the target port. UDP: Port-Unreachable *only
    // when this probe's TTL was NOT limited* (i.e. sent at full TTL to the
    // real destination) is ambiguous with the TTL-limited case below by
    // design in classic UDP traceroute — see probe_udp.hpp when it exists.
    DirectReply,
    // An intermediate hop's TTL-limited response — proves ONLY that this
    // responder sits at this hop count on this destination's path, not
    // that it's the final destination. ICMP/TCP/UDP: Time Exceeded, always
    // ICMP regardless of the probe's own protocol (see doc comment below).
    HopReply,
};

struct ProbeReply {
    ProbeOutcome outcome;
    std::string from;         // responder IP (textual)
    double at = 0;             // epoch seconds
    double rtt_ms = 0;
    // Echoes back whatever the caller passed as `seq` to send_hop_probe/
    // send_direct_probe for the specific probe this reply/completion
    // belongs to. Only meaningful for poll-based strategies (TCP —
    // ProbeTcp::poll_completions) that don't route through the shared
    // ICMP inbox at all, where a probe's identity would otherwise be lost
    // between "sent" and "completed": those two events aren't tied
    // together by anything else the caller can observe. 0 for
    // implementations that don't need it (ICMP/UDP correlate a different
    // way — ICMP via icmp_id_/registry, UDP via the destination-port
    // encoding — see probe_udp.hpp).
    uint16_t seq = 0;
    std::vector<uint32_t> mpls_labels; // RFC 4950, when present — see ParsedReply in icmp.hpp
    // TCP-specific: was this DirectReply a completed handshake (the port is
    // genuinely open) or a refused connection (RST — the host answered, but
    // nothing is listening on this port)? Both equally prove reachability
    // for measurement purposes (see probe_tcp.cpp's own comment on this),
    // but a caller displaying the result to a person should be able to say
    // which actually happened rather than a generic "replied" — this used
    // to be computed in probe_tcp.cpp and then discarded outright.
    // Meaningless (left false) for ICMP/UDP.
    bool tcp_port_open = false;
};

// A ProbeStrategy owns exactly the protocol-specific mechanics: how a probe
// is sent, and how an incoming datagram is recognized as belonging to a
// specific outstanding probe. It does NOT own retry policy, wipe
// thresholds, TTL ramp-up, adoption, or anything else "pillar"-shaped —
// all of that stays in Session, calling through this interface.
//
// IMPORTANT — why hop-discovery replies are always ICMP, even for TCP/UDP:
// an intermediate router that decrements a packet's TTL to zero generates
// an ICMP Time-Exceeded REGARDLESS of what protocol the original packet
// was (TCP, UDP, or ICMP itself) — that's an IP-layer behavior, not a
// transport-layer one. So every ProbeStrategy's hop-discovery replies
// arrive on an ICMP raw socket, identically to today's ICMP mode; only the
// FINAL destination reply (ProbeOutcome::DirectReply) actually differs by
// protocol. Concretely: every ProbeStrategy implementation still needs (or
// shares) an ICMP raw socket for intermediate-hop replies, and layers its
// own protocol-specific final-hop detection on top. This is why
// probe_icmp.hpp's ICMP raw-socket plumbing isn't going away even once TCP
// exists — TCP mode still depends on it for everything except the last hop.
class ProbeStrategy {
public:
    virtual ~ProbeStrategy() = default;

    // Human-readable name for logs/UI — "icmp", "tcp", "udp", "http", "quic".
    virtual const char* name() const = 0;

    // Send one hop-discovery probe at the given TTL/hop-limit. `pin` is an
    // opaque, protocol-chosen flow-pinning token (ICMP: a fixed checksum
    // value: see build_echo's pin_checksum. TCP/UDP: a fixed source port —
    // see probe_tcp.hpp's doc comment for why this is actually a STRICTLY
    // BETTER pin than ICMP's checksum hack, not just a different one).
    // Returns false only on a local send failure (socket error) — never
    // meaningful about whether anything replies.
    virtual bool send_hop_probe(const std::string& dest_ip, uint8_t ttl, uint16_t seq,
                                 size_t payload, uint32_t pin) = 0;

    // Send one direct probe AT the destination itself, full TTL — the
    // "ping" measurement Pillar 2 calls direct-echo. Same pin semantics.
    virtual bool send_direct_probe(const std::string& dest_ip, uint16_t seq,
                                    size_t payload, uint32_t pin) = 0;

    // A fresh, session-scoped flow-pin token — called once per Session at
    // startup (or on rebuild), analogous to icmp_id_ today. What it means
    // is entirely up to the implementation (ICMP: derived into a checksum
    // target; TCP/UDP: a source port to bind every probe socket to).
    virtual uint32_t new_flow_pin() = 0;
};

} // namespace netpulse

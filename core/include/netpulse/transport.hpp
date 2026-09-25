// Raw-socket transport for ICMPv4 / ICMPv6. Cross-platform (POSIX + Winsock);
// the POSIX path is compiled and the codec it relies on is unit-tested. Real
// probing needs privileges.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "netpulse/icmp.hpp"

namespace netpulse {

struct Incoming {
    std::string from; // responder IP (textual)
    ParsedReply reply;
    double at; // epoch seconds
};

// A local network interface address. Historically only ever populated with
// "up, non-loopback" entries (see list_interfaces()'s `include_all` param
// below) for use as a source-address dropdown — `up`/`loopback`/`mtu` exist
// so the SAME struct can also serve the full interfaces-diagnostics page
// (App.jsx's InterfacesPage), which needs to show down/loopback adapters
// too, not just the usable ones. Default member initializers keep every
// existing 3-field aggregate-init call site (`{name, ip, v6}`) valid.
struct NetInterface {
    std::string name;      // adapter name / friendly name
    std::string address;   // local IP (textual)
    bool v6;                // false = IPv4, true = IPv6
    bool up = true;         // operational status; only meaningful when include_all=true — a
                             // default (non-include_all) call already filtered to up-only
    bool loopback = false;  // true for the loopback adapter's own addresses
    uint32_t mtu = 0;       // 0 = unknown/unavailable (best-effort; not every OS/adapter exposes it the same way)
    // FEATURE (user-requested: "add more details" to the interface list/
    // dropdown): best-effort adapter kind — "Wi-Fi", "Ethernet", "Virtual"
    // (VPN/hypervisor/container adapters — vEthernet, VMware, Docker, tun/
    // tap, ...) or "Other" when nothing distinguishes it. This exists
    // because generic driver-assigned names (a Windows GUID-derived name, or
    // a Linux "enp3s0"/"wlp2s0" the user has never had to read before) gave
    // no way to tell which dropdown entry WAS the Wi-Fi adapter without
    // cross-referencing the OS's own network settings — see
    // list_interfaces()'s doc comment below for how each platform derives
    // this.
    std::string kind = "Other";
};

// Enumerate local interfaces. Default (include_all=false, unchanged from
// before this parameter existed): only UP, non-loopback unicast addresses —
// what every existing caller (the source-address dropdown, the
// has_local_v4/has_local_v6 egress check) wants, since a down or loopback
// adapter was never a usable egress. Pass include_all=true for a full
// diagnostic listing (every adapter, up or down, including loopback) with
// `up`/`loopback`/`mtu` populated meaningfully — used by the interfaces
// diagnostics page, not by any probing decision.
std::vector<NetInterface> list_interfaces(bool include_all = false);

// The local address the OS routing table would use to reach `dest_ip` —
// see transport.cpp's doc comment on the implementation for the full
// rationale (it's what lets private/CGNAT hop IPs be safely added to the
// cross-target shared-hop cache, session.cpp). std::nullopt if it can't be
// determined (no route, sandboxed environment, malformed dest_ip, ...).
std::optional<std::string> local_egress_ip(Family family, const std::string& dest_ip);

// One PooledSocket owns one raw socket for a (family, privileged, source)
// combination and is SHARED — via acquire_pooled_socket() (session.cpp) — by
// every target that needs that exact combination, instead of one socket per
// target. Two consequences of being shared, both handled here:
//   1. send() is called concurrently by multiple target threads. sendto()
//      itself is safe to call concurrently on one socket (each call is an
//      atomic datagram write), but IP_TTL/IPV6_UNICAST_HOPS is SOCKET-level
//      state set via setsockopt() immediately before sendto() — not a
//      per-packet parameter — so two threads could interleave {set TTL A}
//      {set TTL B} {both send, one with the wrong TTL}. send_mtx_ wraps
//      exactly that pair as one critical section.
//   2. Nothing here calls recvfrom() anymore — a single process-wide RX
//      dispatcher thread (session.cpp) is the sole reader for every pooled
//      socket, select()-ing across all of them and routing each reply to its
//      owning Session by ICMP id. drain_ready() is what that dispatcher calls:
//      a non-blocking read of whatever is currently queued, since the
//      dispatcher's own select() is what already knows this fd is readable.
class Prober {
public:
    // `source` optionally binds the socket to a local address so probes leave a
    // specific interface (multi-homed hosts, VPNs, etc.).
    Prober(Family family, bool privileged, const std::string& source = std::string());
    ~Prober();
    Prober(const Prober&) = delete;
    Prober& operator=(const Prober&) = delete;

    bool ok() const { return fd_ >= 0; }
    const std::string& error() const { return error_; }
    Family family() const { return family_; }
    long long fd() const { return fd_; } // for the shared RX dispatcher's select() set

    // Send one echo request to a numeric IP with the given TTL / hop limit.
    // `pin_checksum` (IPv4 only) forces the packet's checksum to a fixed value
    // regardless of `seq` — see build_echo()'s doc comment (icmp.hpp) for why.
    // Thread-safe: callable concurrently from any number of sessions sharing
    // this pooled socket (see the class comment above).
    bool send(const std::string& ip, uint8_t ttl, uint16_t id, uint16_t seq, size_t payload,
              std::optional<uint16_t> pin_checksum = std::nullopt);

    // Non-blocking: read and parse every datagram currently queued, return
    // immediately once none remain. Called only by the shared RX dispatcher.
    std::vector<Incoming> drain_ready();

private:
    Family family_;
    long long fd_ = -1;
    std::string error_;
    std::mutex send_mtx_; // serializes {setsockopt TTL, sendto} — see class comment
    void set_nonblocking();
};

// Process-global pool of Prober (pooled-socket) instances, keyed by
// (family, privileged, source_addr). Collapses "one socket per target" (100
// targets => 100 sockets) down to "one socket per DISTINCT combination
// actually in use" — typically 1-2, exactly matching the common case of all
// targets on the default route / raw mode / auto family — while multi-homed
// or VPN setups (different targets bound to different NICs, or mixing
// raw/unprivileged) still get their own socket per distinct combo, so that
// existing, real, user-facing interface/privilege-mode switching keeps
// working unchanged.
//
// Ref-counted via shared_ptr — the returned pointer keeps the pooled socket
// alive for as long as ANY caller holds it; once the last owner releases it,
// the underlying Prober is destroyed (socket closed) same as before pooling.
// A dead (all-owners-released) pool entry is lazily REPLACED by the next
// acquire() for that same combination rather than eagerly erased on last
// release, which would otherwise race a concurrent acquire() for the same key
// (the dying entry's cleanup could erase a brand new entry inserted moments
// earlier) — seeing a stale weak_ptr and overwriting it is always safe, since
// it happens under the same lock as the check.
std::shared_ptr<Prober> acquire_pooled_socket(Family family, bool privileged, const std::string& source);

// Snapshot of every currently-alive pooled socket, for the shared RX
// dispatcher (session.cpp) to select() across. Each call takes shared
// ownership (shared_ptr) of the survivors for the duration of that dispatch
// iteration, so a socket can't be destroyed out from under a concurrent
// drain_ready()/fd() call — it only actually goes away once the dispatcher's
// local copy AND every session's copy have all been released.
std::vector<std::shared_ptr<Prober>> list_active_pooled_sockets();

} // namespace netpulse

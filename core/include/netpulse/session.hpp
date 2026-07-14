// Continuous path-monitoring session: periodic traceroute + per-hop ping, with
// focus-window statistics. IPv4 and IPv6. Mirrors the verified Rust core.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "netpulse/icmp.hpp"
#include "netpulse/transport.hpp"
#include "netpulse/stats.hpp"

namespace netpulse {

enum class FamilyPref { Auto, V4, V6 };

struct Settings {
    double probe_interval = 1.0; // seconds
    double trace_interval = 30.0;
    double timeout = 0.0; // 0 = auto (>= interval, >= 1s)
    size_t payload_size = 56;
    uint8_t max_hops = 30;
    FamilyPref family = FamilyPref::Auto;
    bool privileged = true;
    std::optional<double> focus_secs = 600.0;
    std::string source_addr; // optional local IP to bind (choose egress interface)
    std::set<uint8_t> paused_hops; // hops the user paused — skipped in send loop (cuts load)
};

struct NewPoint {
    uint8_t hop;
    double ts;
    std::optional<double> rtt;
};

// Bounded route-discovery window: while the destination hasn't been located,
// probe only up to kDiscoveryWindow hops past the deepest already-resolved
// hop (see the discovery-cadence comment in session.cpp for why). Exposed
// here (not file-local to session.cpp) so compute_max_hop() below — and its
// unit test — can be shared between the real probe loop and tests/test_core.cpp
// without duplicating the constant.
constexpr int kDiscoveryWindow = 3;
// A CONFIRMED routing loop (see the loop-auditor comment in session.cpp)
// freezes the fast discovery window at the loop boundary instead of letting
// it keep sliding out to max_hops chasing the loop's repeating replies. Only
// this many hops past the boundary stay in scope — enough to notice if the
// loop clears, not enough to reopen the "ghost train" the freeze exists to
// prevent.
constexpr int kLoopAuditWindow = 1;

// The max_hop decision run()'s discovery loop makes every round — factored
// out as a pure function (rather than left inline) so it's independently
// unit-testable (tests/test_core.cpp) without a live socket. Once the real
// destination hop is known, the path is exactly 1..dest_hop. Otherwise the
// window slides out from `frontier` UNLESS a routing loop is confirmed
// (loop_at_hop has a value), in which case it freezes at the loop boundary
// instead (see kLoopAuditWindow).
uint8_t compute_max_hop(std::optional<uint8_t> dest_hop, std::optional<uint8_t> loop_at_hop,
                        uint8_t frontier, uint8_t max_hops);

// Public/private address classification — mirrors web/src/bgp.js's
// isPublicIp() exactly. Exposed here (external linkage, defined in
// session.cpp) so both the real probe loop and tests/test_core.cpp can call
// it, and so the shared-hop cache gate below is independently testable. See
// the full rationale in session.cpp next to SharedHopTable.
bool is_public_ip(const std::string& ip);

// Shared-hop cross-target cache (see the full design rationale in
// session.cpp, right above these types' definitions there). Declared here —
// not file-local to session.cpp — purely so tests/test_core.cpp can construct
// its own local SharedHopTable and exercise shared_publish_to/
// shared_adopt_from directly, without touching the process-global singleton
// the real probe loop uses (session.cpp's internal-linkage g_shared()).
struct SharedSample {
    double ts = 0;                // epoch seconds of the published reply
    double rtt = 0;                // ms
    uint64_t owner_session_id = 0; // publishing Session::id_ (adopt only from a DIFFERENT session)
    uint8_t hop = 0;                // the publisher's hop index for this sample, for attribution/debugging
    std::string predecessor;        // the publisher's predecessor address (or "SRC"), redundant with the key but kept for a future debug surface
};
struct SharedHopTable {
    std::shared_mutex mtx; // multiple concurrent adopters (readers) never block each other; publish (writer) is exclusive
    std::unordered_map<std::string, SharedSample> map; // key: source_addr "\x1f" predecessor "\x1f" responder_ip
};
// Publish a real measurement. No-op for private/CGNAT or empty `ip` (see
// is_public_ip) — those are never cross-target-cached.
void shared_publish_to(SharedHopTable& sh, const std::string& src, const std::string& predecessor,
                       const std::string& ip, double ts, double rtt, uint64_t owner_session_id, uint8_t hop);
// Returns a fresh (<= max_age old), different-owner sample's rtt for this
// exact (source, predecessor, responder) edge, if one exists. std::nullopt
// for private/CGNAT/empty `ip`, no entry, same owner, or a stale entry.
std::optional<double> shared_adopt_from(SharedHopTable& sh, const std::string& src, const std::string& predecessor,
                                        const std::string& ip, uint64_t self_session_id, double now, double max_age);

struct Snapshot {
    std::string target;
    std::optional<std::string> dest_ip;
    std::optional<Family> family;
    bool running = false;
    std::optional<std::string> error;
    std::vector<HopStat> hops;
    std::optional<uint8_t> dest_hop;
    std::vector<NewPoint> new_points;
};

class Session {
public:
    Session(uint64_t id, std::string target, Settings settings);
    ~Session();

    uint64_t id() const { return id_; }
    const std::string& target() const { return target_; }

    // Run until *stop becomes true, invoking on_update each round. While
    // *paused is true the loop stays alive but stops probing.
    void run(std::atomic<bool>* stop, std::atomic<bool>* paused,
             const std::function<void(const Snapshot&)>& on_update);

    // exposed for tests
    void resolve();
    const std::optional<std::string>& dest() const { return dest_; }
    const std::optional<Family>& fam() const { return family_; }

    // Live-editable config (interface, probe interval, timeout, payload, …).
    // Safe to call from another thread while run() is executing.
    void update_settings(const Settings& s);
    Settings settings_snapshot() const;

    // Internal plumbing for the process-global RX dispatcher (see
    // rx_dispatch_loop / PooledSocket in session.cpp) — not intended for
    // external callers. Public only because the dispatcher is a free function
    // living outside this class and needs to reach a session's inbox by ICMP
    // id; mirrors resolve() already being public "for tests" in spirit.
    static void dispatch_incoming(const Incoming& inc);

private:
    uint16_t next_seq();
    Snapshot snapshot(bool running, std::vector<NewPoint> new_points) const;

    struct InFlight {
        uint8_t hop;
        double sent_at;
        bool is_trace;
    };

    uint64_t id_;
    std::string target_;
    Settings settings_;
    mutable std::mutex settings_mtx_;
    bool settings_dirty_ = false;  // run() should re-read settings_
    bool rebuild_needed_ = false;  // family/source changed → rebuild socket
    std::optional<std::string> dest_;
    std::optional<Family> family_;
    // Resolved A/AAAA addresses (if any) for the target. Stored so run()
    // can attempt a pragmatic fallback if the preferred family proves
    // unusable at startup (avoids showing false initial loss when a hostname
    // has both records but one family is unreachable).
    std::optional<std::string> resolved_v4_;
    std::optional<std::string> resolved_v6_;
    uint16_t icmp_id_;
    // Fixed per-session IPv4 checksum-pin target (Paris-traceroute-style ECMP
    // flow pinning — see build_echo()'s doc comment in icmp.hpp and the
    // loop-auditor comment in session.cpp for the full rationale). Ignored
    // for IPv6 sends (kernel owns the ICMPv6 checksum).
    uint16_t paris_checksum_target_;
    uint16_t seq_ = 0;
    std::map<uint8_t, HopStats> hops_;
    std::map<uint16_t, InFlight> inflight_;
    std::optional<std::string> error_;
    std::optional<uint8_t> dest_hop_; // hop index of the destination, once reached
    uint8_t max_hop_seen_ = 0;        // furthest hop that has ever responded

    // Every reply this session ever sees arrives here, pushed by the shared RX
    // dispatcher thread (Session::dispatch_incoming, session.cpp) after a
    // process-global registry lookup by ICMP id — this session's own run()
    // never reads a socket directly (see PooledSocket/acquire_pooled_socket),
    // only drains inbox_, so hops_ stays single-threaded (only run()'s own
    // thread ever touches it). inbox_cv_ lets run() block on an empty inbox
    // with a timeout instead of polling, waking immediately the instant the
    // dispatcher delivers something.
    std::mutex inbox_mtx_;
    std::condition_variable inbox_cv_;
    std::deque<Incoming> inbox_;
};

} // namespace netpulse

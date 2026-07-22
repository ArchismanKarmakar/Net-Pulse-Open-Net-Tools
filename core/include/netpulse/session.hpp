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
    // Frankenstein-route guard tuning (see kLegacyMissThreshold/
    // kLegacyMissWindowSecs in session.cpp for the full mechanism). The
    // built-in defaults were tuned against a lossy consumer-ISP path; a
    // datacenter/enterprise deployment watching a fast-failover route may
    // want a much SHORTER window (detect a real reroute sooner), while a
    // deployment on a chronically rate-limited path may want it LONGER
    // (fewer false wipes). 0/0 means "use the built-in default" — these are
    // opt-in overrides, not required fields.
    double legacy_miss_window_secs = 0.0;
    int legacy_miss_threshold = 0;
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

// Anything that can own an ICMP id and receive replies addressed to it via
// the shared registry/RX-dispatcher (see register_icmp_owner below and the
// registry's own doc comment in session.cpp) implements this. Session is the
// original (and still primary) implementor; PingRun (ping_run.hpp) is the
// second — added so the standalone Ping tool shares this exact same pooled-
// socket + single-process-wide-dispatcher-thread infrastructure instead of
// spawning a native OS process per ping and text-parsing its output, which
// is what that used to do. Deliberately a tiny, single-method interface:
// anything more would start pulling Session-specific concepts (hops, focus
// windows, snapshots) into what's meant to be the minimum contract "I have
// an ICMP id, route replies addressed to it to me."
class IcmpOwner {
public:
    virtual ~IcmpOwner() = default;
    virtual void push_incoming(const Incoming& inc) = 0;
};

// Register/unregister an owner for a given ICMP id in the process-global
// registry the shared RX dispatcher (session.cpp) routes every reply
// through. Thread-safe; safe to call from any thread. An id already
// registered to a DIFFERENT owner is silently overwritten (matches the
// pre-existing Session-only registry's behavior) — callers are expected to
// derive sufficiently unique ids (see Session's and PingRun's constructors)
// that this is a non-event in practice, not a real collision path.
void register_icmp_owner(uint16_t icmp_id, IcmpOwner* owner);
// No-op if `owner` is no longer the current registrant for `icmp_id` (e.g.
// it was already superseded) — safe to call unconditionally on teardown.
void unregister_icmp_owner(uint16_t icmp_id, IcmpOwner* owner);

struct Snapshot {
    std::string target;
    std::optional<std::string> dest_ip;
    std::optional<Family> family;
    bool running = false;
    // Reserved for conditions that mean the session can't produce useful hop
    // data at all (DNS resolution failed, no address for the requested
    // family, needs admin/root, etc.) — the UI is expected to treat a
    // non-empty `error` as "show a blocking notice instead of the hop
    // table", so nothing merely advisory belongs here. See loop_warning
    // below for the routing-loop case, which is exactly the opposite: the
    // table is fully valid and should stay visible.
    std::optional<std::string> error;
    // A CONFIRMED routing loop (see the loop-auditor comment in session.cpp)
    // is an advisory, not a fatal condition — every hop's data is still
    // valid and being measured; this only flags that two hops appear to
    // share a physical path. Deliberately separate from `error` so the UI
    // can surface it without hiding the table it's describing.
    // loop_hop/loop_dup_at are the two implicated hop numbers (structured,
    // not parsed out of the message) so the UI can highlight those exact
    // rows; loop_warning is the human-readable form for a banner/tooltip.
    std::optional<std::string> loop_warning;
    std::optional<uint8_t> loop_hop;
    std::optional<uint8_t> loop_dup_at;
    std::vector<HopStat> hops;
    std::optional<uint8_t> dest_hop;
    std::vector<NewPoint> new_points;
};

class Session : public IcmpOwner {
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

    // Manually re-trigger route discovery from scratch without touching
    // family/source/socket (contrast with update_settings()'s rebuild_needed_
    // path, which only fires on an actual family/source/privileged change).
    // Safe to call from another thread while run() is executing — just flags
    // the request; run()'s own thread performs the actual reset next round.
    void force_recheck();

    // Ask run() to rebuild its socket (and re-resolve DNS — see the
    // `rebuild` branch in run()) on its next pass, WITHOUT this being a
    // user-visible "family/source changed" settings edit. Intended for
    // external self-heal signals: the Manager-level network-interface-change
    // watchdog (manager.hpp) calls this on every active session when the
    // OS's interface/address list changes (the common sleep/wake signature),
    // and run()'s own internal dead-socket/silence self-heal (session.cpp)
    // uses the identical rebuild_needed_ flag directly for the same reason.
    // Reuses the exact rebuild path update_settings() already exercises, so
    // there's no new socket/state-reset logic to get wrong — just a new way
    // to ask for the existing one. Safe to call from another thread while
    // run() is executing.
    void request_rebuild();

    // Internal plumbing for the process-global RX dispatcher (see
    // rx_dispatch_loop / PooledSocket in session.cpp) — not intended for
    // external callers. Public only because the dispatcher is a free function
    // living outside this class and needs to reach a session's inbox by ICMP
    // id; mirrors resolve() already being public "for tests" in spirit.
    // Looks the owner up in the shared registry (now IcmpOwner-typed, not
    // Session-specific — see register_icmp_owner above) and forwards via
    // push_incoming(). Kept as the dispatcher's call site (unchanged
    // signature) so RxDispatcher itself needed no changes when the registry
    // was generalized.
    static void dispatch_incoming(const Incoming& inc);
    // IcmpOwner
    void push_incoming(const Incoming& inc) override;

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
    std::atomic<bool> force_recheck_needed_{false}; // user-requested: fire the same due_recheck mechanism now instead of waiting for kHopRecheckSecs
    // Last time we sent the deliberate "is the destination now CLOSER than
    // dest_hop_?" probe at ttl = dest_hop_ - 1 (see the dest_hop_ shrink
    // check in try_send/session.cpp) — tracked separately from per-hop
    // hop_recheck_at so it doesn't interfere with that hop's own bookkeeping.
    double dest_shrink_check_at_ = 0.0;
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
    // Routing-loop advisory state — see the Snapshot fields' doc comments for
    // why this is separate from error_. Members (not run()-local) for the
    // same reason error_ isn't local: snapshot() reads them directly.
    std::optional<std::string> loop_warning_;
    std::optional<uint8_t> loop_hop_;
    std::optional<uint8_t> loop_dup_at_;
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
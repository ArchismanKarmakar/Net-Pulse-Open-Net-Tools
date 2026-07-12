// Continuous path-monitoring session: periodic traceroute + per-hop ping, with
// focus-window statistics. IPv4 and IPv6. Mirrors the verified Rust core.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
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
    uint16_t seq_ = 0;
    std::map<uint8_t, HopStats> hops_;
    std::map<uint16_t, InFlight> inflight_;
    std::optional<std::string> error_;
    std::optional<uint8_t> dest_hop_; // hop index of the destination, once reached
    uint8_t max_hop_seen_ = 0;        // furthest hop that has ever responded

    // Cross-session reply routing. On Windows, when several targets each open a
    // raw ICMP socket, inbound ICMP replies are often delivered to only ONE of
    // the sockets, so a reply for target B can arrive on target A's socket and
    // be discarded (wrong id) — leaving B's shared hops (router/BNG) at high
    // loss even though the replies came back. To fix this, every session
    // registers its ICMP id in a process-global table; a session that receives a
    // reply not addressed to it hands it to the owning session's inbox, which
    // that session drains on its own thread (so hops_ stays single-threaded).
    std::mutex inbox_mtx_;
    std::deque<Incoming> inbox_;
    void route_reply_(const Incoming& inc); // deliver a foreign reply to its owner
};

} // namespace netpulse

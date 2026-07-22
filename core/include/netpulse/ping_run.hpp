// ping_run.hpp — the standalone Ping tool's engine. Deliberately built on
// the EXACT SAME shared infrastructure the main multi-target engine uses —
// the pooled socket (acquire_pooled_socket, transport.hpp) and the single
// process-wide RX dispatcher thread (session.cpp) — via the IcmpOwner
// interface (session.hpp), instead of spawning the OS `ping` binary and
// text-parsing its (locale- and platform-dependent) stdout, which is what
// this replaces. Two concrete things that buys, beyond just "one fewer
// external dependency":
//   1. Scalability story stays uniform. A Ping tool run doesn't open its own
//      raw socket or spawn its own OS process — it shares whichever pooled
//      socket the main engine already has open for that (family, privileged,
//      source) combination, and is read by the SAME single dispatcher thread
//      regardless of how many targets or concurrent pings are active. 100
//      monitored targets plus a Ping-tool run in another tab still costs
//      exactly one extra raw socket in the worst case (a family/privilege/
//      source combo nothing else is currently using), not one per ping.
//   2. No text-parsing fragility. Every field (RTT, source address, reply
//      kind) comes from the SAME parsed ICMP reply structures (ParsedReply,
//      icmp.hpp) the main engine already relies on — nothing regex-matches
//      another program's stdout, so there's no locale/OS-version/ping-
//      variant format to keep up with.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "netpulse/session.hpp" // IcmpOwner, register/unregister_icmp_owner
#include "netpulse/transport.hpp"

namespace netpulse {

enum class PingFamilyPref { Auto, V4, V6 };

struct PingConfig {
    std::string target;
    PingFamilyPref family = PingFamilyPref::Auto;
    bool privileged = true;
    std::string source_addr;
    int count = 10;             // ignored when continuous is true
    bool continuous = false;
    size_t payload_size = 56;
    double interval_secs = 1.0;
    double timeout_secs = 0.0;  // 0 = auto (interval, floored to 1s — mirrors Session's own timeout=0 convention)
    uint8_t ttl = 255;          // full TTL: a ping measures the DESTINATION directly, not an intermediate hop
};

// One reply, timeout, or fatal condition for a single sequence number.
// `note` carries a short human-readable reason for anything that isn't a
// successful reply (e.g. "Request timed out", "Destination unreachable",
// a resolve/socket error) — the ONE piece of free text in this struct,
// everything else the UI needs is a typed field.
struct PingLine {
    int seq = 0;
    bool ok = false;
    std::optional<double> rtt_ms;
    std::string from_ip;
    std::string note;
};

// One run of the Ping tool. Not thread-safe to drive from multiple threads
// at once — mirrors Session: exactly one owner thread calls run() (see
// Manager::start_ping in manager.hpp), and results are delivered through
// on_line/on_done callbacks invoked from THAT thread; a caller needing
// cross-thread visibility synchronizes on its own side, the same pattern
// WebTarget already uses for Session's on_update.
class PingRun : public IcmpOwner {
public:
    PingRun(uint64_t id, PingConfig cfg);
    ~PingRun() override;

    // Runs until *stop is set, or (for a non-continuous run) every
    // configured probe has either been answered or timed out. on_line fires
    // once per sequence number (a real reply, a timeout, or a send
    // failure); on_done fires exactly once, at the very end, whether the
    // run completed naturally or was stopped early.
    void run(std::atomic<bool>* stop,
             const std::function<void(const PingLine&)>& on_line,
             const std::function<void()>& on_done);

    // IcmpOwner
    void push_incoming(const Incoming& inc) override;

private:
    bool resolve();

    uint64_t id_;
    PingConfig cfg_;
    uint16_t icmp_id_;
    std::optional<std::string> dest_;
    std::optional<Family> family_;

    std::mutex inbox_mtx_;
    std::condition_variable inbox_cv_;
    std::deque<Incoming> inbox_;
};

} // namespace netpulse
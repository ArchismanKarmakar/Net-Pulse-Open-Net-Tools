// Per-hop rolling samples and focus-window statistics.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace netpulse {

double now_secs();

// (epoch seconds, rtt in ms or nullopt for a lost probe)
using Sample = std::pair<double, std::optional<double>>;

// How long a hop's samples stay in RAM before aging out. Fixed and small,
// independent of probe rate or how long a target has been running — this is
// the actual RAM-scaling lever for 100+ long-running targets: bounded window
// per hop instead of the old count-based cap (up to 100,000 samples/hop,
// sized to cover a full 24h at fast probe rates). Long-window/"all time"
// queries are served by ColdStore below instead of by keeping more in RAM.
constexpr double kHotWindowSecs = 900.0; // 15 minutes

// Running accumulator over a range of on-disk cold-tier records — enough to
// merge with a hot-tier rtt list to answer sent/recv/loss/min/max/avg/std
// without needing the full point list back. No median/jitter: those stay
// hot-tier-only (recent-window precision), same tradeoff most APM tools make
// for long lookback windows — see compute_stat()'s cold-merge comment in
// manager.hpp for where this is actually consumed.
struct ColdAggregate {
    size_t sent = 0, recv = 0;
    double sum = 0.0, sumsq = 0.0, minv = 0.0, maxv = 0.0;
};

// Background-flushed, per-(target,hop) append-only binary sample log that
// backs long-focus-window / "All" queries once a hop's data has aged out of
// its RAM hot tier (WebTarget::series in manager.hpp is the store this
// backs). Writes happen on a single dedicated worker thread — mirrors the
// reverse-DNS resolver's background-worker pattern (session.cpp's
// RdnsResolver/g_rdns()): a producer enqueues work and returns immediately,
// a detached process-lifetime thread drains the queue, so disk I/O never
// blocks whichever thread is evicting hot-tier samples. Reads are cached per
// (target,hop) and only actually touch disk every kColdRecomputeSecs (see
// stats.cpp), so calling aggregate()/read_range() from a frequent polling
// path doesn't turn into a disk read on every call.
class ColdStore {
public:
    static ColdStore& instance();

    // Sets the on-disk root ("<dir>/stats/<target_id>/<hop>.bin" per hop).
    // Call once, early (before any target starts probing), once a host
    // knows its real data directory. If never called, the first actual use
    // self-configures a relative "npdata" default so history isn't silently
    // dropped in the meantime.
    void configure(const std::string& dir);

    // Hand off a batch of samples aging out of a hop's hot tier. Copies the
    // data and returns immediately — the write happens on the worker thread.
    void enqueue_flush(uint64_t target_id, uint8_t hop, std::vector<Sample> batch);

    // Aggregate of every cold record with ts >= cutoff (or every record ever
    // written, if !cutoff).
    ColdAggregate aggregate(uint64_t target_id, uint8_t hop, std::optional<double> cutoff);

    // Raw cold records with ts >= cutoff — for extending a chart line
    // further back than the hot tier holds.
    std::vector<Sample> read_range(uint64_t target_id, uint8_t hop, std::optional<double> cutoff);

    // A route flap (a different device now answers at this hop) means the
    // old device's history no longer describes what's there now — drop its
    // cold file too, not just the RAM hot tier.
    void reset(uint64_t target_id, uint8_t hop);

private:
    ColdStore() = default;

    struct FlushJob {
        uint64_t target_id;
        uint8_t hop;
        std::vector<Sample> batch;
    };
    struct CacheEntry {
        double computed_at = -1e18;
        bool has_cutoff = false;
        double cutoff = 0.0;
        ColdAggregate agg;
        std::vector<Sample> range;
    };

    void ensure_configured();
    void ensure_worker_started();
    void worker_loop();
    std::string path_for(uint64_t target_id, uint8_t hop);
    // Caller must hold cache_mtx_. Re-reads from disk if the cache entry is
    // missing, stale (older than kColdRecomputeSecs), or for a different
    // cutoff than last time.
    CacheEntry& get_or_refresh(uint64_t target_id, uint8_t hop, std::optional<double> cutoff);
    void refresh(uint64_t target_id, uint8_t hop, std::optional<double> cutoff, CacheEntry& out);

    std::mutex dir_mtx_;
    std::string dir_;
    bool configured_ = false;

    std::mutex cache_mtx_;
    std::map<std::pair<uint64_t, uint8_t>, CacheEntry> cache_;

    std::mutex qmtx_;
    std::condition_variable qcv_;
    std::deque<FlushJob> queue_;
    bool worker_started_ = false;
};

struct HopStat {
    uint8_t hop = 0;
    std::optional<std::string> address;
    std::optional<std::string> hostname;
    bool is_dest = false;
    size_t sent = 0;
    size_t recv = 0;
    double loss = 0.0;
    std::optional<double> cur, min, max, avg;
    double std = 0.0;
    // Last-known MPLS label stack (RFC 4950), top-of-stack first — see
    // ParsedReply::mpls_labels. A statistic in name only: unlike loss/rtt this
    // isn't aggregated over the focus window, it's just the most recent
    // non-empty stack this hop has reported.
    std::vector<uint32_t> mpls_labels;
    // Only populated when `address` above is empty (a hop currently showing
    // `*`) AND this hop has answered at some point in the past — the last
    // address/hostname it had, and when it was last actually confirmed
    // (wall-clock unix seconds), so the UI can show "117.216.207.209 (stale,
    // last seen 4m ago)" instead of nothing at all. See HopStats::
    // clear_address()'s doc comment for why this survives a guarded wipe
    // when the live address/hostname/history do not.
    std::optional<std::string> stale_address;
    std::optional<std::string> stale_hostname;
    std::optional<double> stale_since;
};

class HopStats {
public:
    // target_id disambiguates cold-tier files across targets that share the
    // same hop numbering (e.g. every target's hop 1 is a different device) —
    // without it, two targets' hop-1 history would collide in the same file.
    HopStats(uint64_t target_id, uint8_t hop) : target_id_(target_id), hop_(hop) {}

    uint8_t hop() const { return hop_; }
    const std::optional<std::string>& address() const { return address_; }
    bool is_dest() const { return is_dest_; }
    void set_is_dest(bool v) { is_dest_ = v; }
    const std::optional<std::string>& hostname() const { return hostname_; }
    void set_hostname(std::string h) { hostname_ = std::move(h); }
    const std::vector<uint32_t>& mpls_labels() const { return mpls_labels_; }
    void set_mpls_labels(std::vector<uint32_t> labels) { mpls_labels_ = std::move(labels); }

    // Assign / change the hop's IP. A real change (route flap) clears history.
    void set_address(const std::string& addr);
    // Reset to unknown (*) — used when a route change is suspected (the new
    // router silently drops legacy/TTL-limited probes instead of replying
    // with a different address) but never confirmed via set_address(), so
    // the old device's identity would otherwise linger forever even though
    // it's no longer actually on the path. See session.cpp's legacy_miss.
    // Preserves the about-to-be-cleared address/hostname into
    // stale_address()/stale_hostname() first (see those doc comments) —
    // "reset to unknown" for measurement purposes shouldn't also mean
    // "forget what was last here" for display purposes.
    void clear_address();
    void push(double ts, std::optional<double> rtt);
    HopStat compute(std::optional<double> focus_secs) const;

    // Last address/hostname this hop had before going dark, and when it was
    // last actually confirmed (a real reply, not a loss) — nullopt/0 if
    // this hop has never once answered. Deliberately NOT cleared by
    // clear_address() (that's the whole point — see its doc comment); only
    // superseded once a live address exists again (see set_address()).
    // last_confirmed_at() keeps updating on every real reply regardless of
    // whether the CURRENT address is live or stale, so it's always "when
    // did we last genuinely hear from whatever's/was at this position."
    const std::optional<std::string>& stale_address() const { return stale_address_; }
    const std::optional<std::string>& stale_hostname() const { return stale_hostname_; }
    double last_confirmed_at() const { return last_confirmed_at_; }

private:
    uint64_t target_id_;
    uint8_t hop_;
    std::optional<std::string> address_;
    std::optional<std::string> hostname_;
    bool is_dest_ = false;
    std::vector<uint32_t> mpls_labels_;
    std::deque<Sample> samples_; // hot tier only — bounded to kHotWindowSecs, see push()
    std::optional<std::string> stale_address_;
    std::optional<std::string> stale_hostname_;
    double last_confirmed_at_ = 0.0;
};

} // namespace netpulse
#include "netpulse/stats.hpp"
#include "netpulse/session.hpp"
// BUG FIX (see enqueue_flush() below): debug_log() lives here, and this file
// never included it before — the exact "accidentally relies on some OTHER
// file transitively pulling it in" trap already found and fixed once this
// session for netpulse_ffi.cpp. Explicit here, not assumed.
#include "netpulse/platform.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>
#include <vector>

namespace netpulse {

double now_secs() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

// How often a (target,hop)'s cold-tier cache entry is allowed to go stale
// before the next aggregate()/read_range() call actually re-touches disk —
// decouples cold-tier cost from the UI's ~600ms poll cadence.
constexpr double kColdRecomputeSecs = 5.0;

// -------------------------------------------------------------- HopStats

void HopStats::set_address(const std::string& addr) {
    if (!address_ || *address_ != addr) {
        // Only a genuine flap (a DIFFERENT device replacing a previously
        // known one) invalidates history — the very first address assignment
        // for a freshly-discovered hop has no prior device's data to drop.
        bool flap = address_.has_value();
        address_ = addr;
        hostname_.reset();
        samples_.clear();
        mpls_labels_.clear(); // a route flap means a different device — its old label stack no longer applies
        local_port_ = 0; // same reasoning — stale for a different device at this hop
        tcp_port_open_.reset(); // same reasoning
        // A live address is about to be shown again — whatever was
        // remembered as "stale" (see clear_address()) is superseded, whether
        // this is the same device coming back or a genuinely different one.
        stale_address_.reset();
        stale_hostname_.reset();
        // The old device's on-disk history no longer describes what's at
        // this hop now — drop its cold file too, not just the RAM hot tier.
        if (flap) ColdStore::instance().reset(target_id_, hop_);
    }
}

void HopStats::clear_address() {
    bool had_address = address_.has_value();
    if (had_address) {
        // Preserve what was last here for display purposes BEFORE wiping the
        // live fields below — see stale_address()'s doc comment in
        // stats.hpp. last_confirmed_at_ is deliberately left untouched here:
        // it already holds the last time THIS address actually replied (set
        // in push(), never by clear_address()), which is exactly the
        // timestamp a "stale, last seen Xm ago" display needs — the moment
        // of the wipe itself is not a confirmation of anything.
        stale_address_ = address_;
        stale_hostname_ = hostname_;
    }
    address_.reset();
    hostname_.reset();
    samples_.clear();
    mpls_labels_.clear();
    local_port_ = 0;
    tcp_port_open_.reset();
    // Same reasoning as the flap branch in set_address() above: whatever was
    // here is no longer confirmed to be on the path, so its history no
    // longer describes what's (or isn't) at this hop.
    if (had_address) ColdStore::instance().reset(target_id_, hop_);
}

void HopStats::push(double ts, std::optional<double> rtt) {
    if (rtt.has_value()) last_confirmed_at_ = ts; // a REAL reply — see last_confirmed_at()'s doc comment
    samples_.emplace_back(ts, rtt);
    // Hot tier is bounded by TIME (kHotWindowSecs), not count — this is what
    // lets RAM stay flat regardless of probe rate or how long a target has
    // run. Whatever ages out gets handed to the cold store instead of just
    // being dropped, so long-window queries still see it (see compute()).
    double cutoff = ts - kHotWindowSecs;
    std::vector<Sample> aged;
    while (!samples_.empty() && samples_.front().first < cutoff) {
        aged.push_back(samples_.front());
        samples_.pop_front();
    }
    if (!aged.empty()) ColdStore::instance().enqueue_flush(target_id_, hop_, std::move(aged));
}

HopStat HopStats::compute(std::optional<double> focus_secs) const {
    std::optional<double> cutoff;
    if (focus_secs) cutoff = now_secs() - *focus_secs;

    HopStat s;
    s.hop = hop_;
    s.address = address_;
    s.hostname = hostname_;
    s.is_dest = is_dest_;
    s.mpls_labels = mpls_labels_;
    s.local_port = local_port_;
    s.tcp_port_open = tcp_port_open_;
    // Only worth surfacing when there's no LIVE address to show instead —
    // see the HopStat doc comment in stats.hpp.
    if (!address_ && stale_address_) {
        s.stale_address = stale_address_;
        s.stale_hostname = stale_hostname_;
        if (last_confirmed_at_ > 0.0) s.stale_since = last_confirmed_at_;
        // Same table shared_adopt_from() reads from in session.cpp's send
        // loop. 30s is generous relative to typical probe intervals
        // (default 1s), so this stays "is anyone hearing from it right
        // now", not a long-tail memory of an address that's genuinely
        // gone quiet everywhere.
        s.stale_seen_elsewhere_at = shared_last_seen(*stale_address_, now_secs(), 30.0);
    }

    double hot_sum = 0, hot_sumsq = 0, hot_min = 0, hot_max = 0;
    bool hot_has = false;
    size_t sent = 0, hot_recv = 0;
    std::optional<double> cur;
    for (const auto& [ts, rtt] : samples_) {
        if (cutoff && ts < *cutoff) continue;
        ++sent;
        cur = rtt; // latest (possibly nullopt = lost)
        if (rtt) {
            ++hot_recv;
            if (!hot_has) { hot_min = hot_max = *rtt; hot_has = true; }
            else { hot_min = std::min(hot_min, *rtt); hot_max = std::max(hot_max, *rtt); }
            hot_sum += *rtt;
            hot_sumsq += (*rtt) * (*rtt);
        }
    }
    // Merge in the cold-tier aggregate for whatever part of the focus window
    // (if any) reaches further back than the hot tier holds. Un-focused
    // ("all time") queries always need this once a hop has run past
    // kHotWindowSecs. Cached inside ColdStore — see kColdRecomputeSecs.
    // Individual cold rtts aren't kept (only sum/sumsq/min/max), so std is
    // computed via the merged mean/sumsq formula below; `cur` never comes
    // from cold data since it's always older than every hot sample.
    bool need_cold = !cutoff || samples_.empty() || *cutoff < samples_.front().first;
    ColdAggregate cold;
    if (need_cold) cold = ColdStore::instance().aggregate(target_id_, hop_, cutoff);
    sent += cold.sent;
    size_t total_recv = hot_recv + cold.recv;

    s.sent = sent;
    s.recv = total_recv;
    s.loss = sent > 0 ? (1.0 - static_cast<double>(total_recv) / sent) * 100.0 : 0.0;
    s.cur = cur;
    if (total_recv > 0) {
        double sum = hot_sum + cold.sum;
        double sumsq = hot_sumsq + cold.sumsq;
        double mn = hot_has ? hot_min : cold.minv;
        double mx = hot_has ? hot_max : cold.maxv;
        if (hot_has && cold.recv > 0) {
            mn = std::min(mn, cold.minv);
            mx = std::max(mx, cold.maxv);
        }
        double avg = sum / total_recv;
        double var = sumsq / total_recv - avg * avg;
        s.min = mn;
        s.max = mx;
        s.avg = avg;
        s.std = std::sqrt(std::max(0.0, var));
    }
    return s;
}

// -------------------------------------------------------------- ColdStore

ColdStore& ColdStore::instance() {
    static ColdStore inst;
    return inst;
}

void ColdStore::configure(const std::string& dir) {
    std::lock_guard<std::mutex> lk(dir_mtx_);
    dir_ = dir;
    configured_ = true;
}

void ColdStore::ensure_configured() {
    std::lock_guard<std::mutex> lk(dir_mtx_);
    if (!configured_) { dir_ = "npdata"; configured_ = true; }
}

std::string ColdStore::path_for(uint64_t target_id, uint8_t hop) {
    ensure_configured();
    std::string root;
    { std::lock_guard<std::mutex> lk(dir_mtx_); root = dir_; }
    // create_directories builds the whole tree in one call (root/stats/<id>
    // may all be missing at once, e.g. on first run with a fresh data dir) —
    // a plain mkdir() only ever creates one level and silently no-ops if its
    // own parent doesn't exist yet, which would leave fopen() failing on
    // every write/read with no obvious cause.
    std::filesystem::path tgtDir = std::filesystem::path(root) / "stats" / std::to_string(target_id);
    std::error_code ec;
    std::filesystem::create_directories(tgtDir, ec);
    return (tgtDir / (std::to_string(int(hop)) + ".bin")).string();
}

void ColdStore::ensure_worker_started() {
    std::lock_guard<std::mutex> lk(qmtx_);
    if (worker_started_) return;
    worker_started_ = true;
    std::thread([this]() { worker_loop(); }).detach();
}

void ColdStore::enqueue_flush(uint64_t target_id, uint8_t hop, std::vector<Sample> batch) {
    if (batch.empty()) return;
    ensure_worker_started();
    {
        std::lock_guard<std::mutex> lk(qmtx_);
        // BUG FIX: this queue had no size cap at all. It's serviced by a
        // SINGLE background thread for every target and every hop in the
        // whole process — fine under light load, but if disk I/O ever falls
        // behind the rate hot-tier samples age out at (a slow disk, or
        // real-time antivirus scanning intercepting every file write — a
        // live report specifically flagged Kaspersky actively monitoring
        // this process), jobs pile up here forever. Each carries its own
        // std::vector<Sample> payload, so this is genuinely unbounded RAM
        // growth over a long enough session, not just a queued backlog —
        // exactly the class of thing that looks fine in short testing and
        // degrades hours into a real deployment. Capped here rather than
        // blocking: blocking would stall the CALLING thread, which is a
        // live probe session's own loop, turning a stats-persistence
        // slowdown into a probing slowdown, which is worse. Dropping the
        // OLDEST pending job trades a little historical cold-tier detail
        // (never live data — flush jobs are already-aged-out samples) for
        // guaranteed bounded memory; logged once so a real backlog is
        // diagnosable rather than silently lossy forever.
        if (queue_.size() >= kMaxQueuedFlushJobs) {
            queue_.pop_front();
            if (!drop_notice_shown_) {
                drop_notice_shown_ = true;
                debug_log("[netpulse] ColdStore flush queue exceeded its cap (" +
                          std::to_string(kMaxQueuedFlushJobs) +
                          ") -- the persistence worker is falling behind (slow disk, or "
                          "AV real-time scanning?). Dropping oldest pending jobs to keep "
                          "memory bounded; live stats are unaffected.\n");
            }
        }
        queue_.push_back(FlushJob{target_id, hop, std::move(batch)});
    }
    qcv_.notify_one();
}

void ColdStore::worker_loop() {
    for (;;) {
        FlushJob job;
        {
            std::unique_lock<std::mutex> lk(qmtx_);
            qcv_.wait(lk, [this] { return !queue_.empty(); });
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        std::string path = path_for(job.target_id, job.hop);
        std::FILE* f = std::fopen(path.c_str(), "ab");
        if (!f) continue;
        for (const auto& [ts, rtt] : job.batch) {
            double rec[2] = {ts, rtt ? *rtt : std::nan("")};
            std::fwrite(rec, sizeof(double), 2, f);
        }
        std::fclose(f);
    }
}

void ColdStore::refresh(uint64_t target_id, uint8_t hop, std::optional<double> cutoff, CacheEntry& out) {
    out.computed_at = now_secs();
    out.has_cutoff = cutoff.has_value();
    out.cutoff = cutoff.value_or(0.0);
    out.agg = ColdAggregate{};
    out.range.clear();

    std::string path = path_for(target_id, hop);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;

    constexpr size_t kRec = sizeof(double) * 2;
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    size_t n = fsize > 0 ? static_cast<size_t>(fsize) / kRec : 0;

    size_t start = 0;
    if (cutoff && n > 0) {
        // Records are timestamp-ordered — binary search by offset for the
        // first record with ts >= cutoff, same technique focus_begin() uses
        // in RAM (manager.hpp), just via fseek instead of an iterator.
        size_t lo = 0, hi = n;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            double rec[2];
            std::fseek(f, static_cast<long>(mid * kRec), SEEK_SET);
            std::fread(rec, sizeof(double), 2, f);
            if (rec[0] < *cutoff) lo = mid + 1; else hi = mid;
        }
        start = lo;
    }

    bool first = true;
    std::fseek(f, static_cast<long>(start * kRec), SEEK_SET);
    for (size_t i = start; i < n; ++i) {
        double rec[2];
        if (std::fread(rec, sizeof(double), 2, f) != 2) break;
        double ts = rec[0];
        std::optional<double> rtt = std::isnan(rec[1]) ? std::nullopt : std::optional<double>(rec[1]);
        out.range.emplace_back(ts, rtt);
        ++out.agg.sent;
        if (rtt) {
            ++out.agg.recv;
            out.agg.sum += *rtt;
            out.agg.sumsq += (*rtt) * (*rtt);
            if (first) { out.agg.minv = out.agg.maxv = *rtt; first = false; }
            else { out.agg.minv = std::min(out.agg.minv, *rtt); out.agg.maxv = std::max(out.agg.maxv, *rtt); }
        }
    }
    std::fclose(f);
}

ColdStore::CacheEntry& ColdStore::get_or_refresh(uint64_t target_id, uint8_t hop, std::optional<double> cutoff) {
    auto key = std::make_pair(target_id, hop);
    CacheEntry& e = cache_[key];
    bool stale = (now_secs() - e.computed_at) >= kColdRecomputeSecs;
    bool cutoff_changed = e.has_cutoff != cutoff.has_value() ||
                          (cutoff && std::abs(e.cutoff - *cutoff) > 0.001);
    if (e.computed_at < -1e17 || stale || cutoff_changed) refresh(target_id, hop, cutoff, e);
    return e;
}

ColdAggregate ColdStore::aggregate(uint64_t target_id, uint8_t hop, std::optional<double> cutoff) {
    std::lock_guard<std::mutex> lk(cache_mtx_);
    return get_or_refresh(target_id, hop, cutoff).agg;
}

std::vector<Sample> ColdStore::read_range(uint64_t target_id, uint8_t hop, std::optional<double> cutoff) {
    std::lock_guard<std::mutex> lk(cache_mtx_);
    return get_or_refresh(target_id, hop, cutoff).range;
}

void ColdStore::reset(uint64_t target_id, uint8_t hop) {
    std::string path = path_for(target_id, hop);
    std::remove(path.c_str());
    std::lock_guard<std::mutex> lk(cache_mtx_);
    cache_.erase(std::make_pair(target_id, hop));
}

void ColdStore::forget_target(uint64_t target_id) {
    // See this method's own doc comment (stats.hpp) for why it needs to
    // exist at all. All of a target's hop files live under one directory
    // (path_for()'s own convention: "<root>/stats/<target_id>/<hop>.bin"),
    // so removing that whole directory in one call is both simpler and
    // more complete than reset()'s per-hop approach — it also catches any
    // hop this target ever used, not just the ones happening to still be in
    // cache_ right now.
    {
        ensure_configured(); // same lazy-default pattern path_for() uses
        std::string root;
        { std::lock_guard<std::mutex> lk(dir_mtx_); root = dir_; }
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(root) / "stats" / std::to_string(target_id), ec);
        // Best-effort: a failure here (file locked, permissions) just means
        // stale on-disk data for a removed target lingers a little longer,
        // not a functional problem — the in-memory cache_ erase below is
        // what actually matters for the leak this exists to fix.
    }
    std::lock_guard<std::mutex> lk(cache_mtx_);
    // std::map<std::pair<uint64_t,uint8_t>, ...> keys sort target_id-major,
    // so every entry for this target forms one contiguous range — erase it
    // in one call rather than a linear scan-and-erase over the whole map.
    auto lo = cache_.lower_bound(std::make_pair(target_id, uint8_t{0}));
    auto hi = cache_.upper_bound(std::make_pair(target_id, std::numeric_limits<uint8_t>::max()));
    cache_.erase(lo, hi);
}

} // namespace netpulse
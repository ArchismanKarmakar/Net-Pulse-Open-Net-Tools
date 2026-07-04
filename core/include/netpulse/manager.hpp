// NetPulse target Manager — the shared engine + per-target state/series + JSON
// state production. Used by BOTH the HTTP server (server/main.cpp) and the
// Node-API addon (napi/napi.cpp), so they drive the identical verified core.
#pragma once

#include "netpulse/session.hpp"
#include "netpulse/stats.hpp"
#include "netpulse/transport.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__linux__)
#  include <sys/resource.h>
#endif

namespace netpulse {

// Samples are appended in increasing-timestamp order (see WebTarget series
// push logic), so finding "the first sample at or after `cutoff`" is a binary
// search, not a linear scan. compute_stat() and the chart downsampler below
// both used to `for (auto& s : pts) if (s.first < cutoff) continue;` — a scan
// of the ENTIRE historical buffer on every single poll (every ~600ms),
// regardless of how much history had piled up. That cost grows without bound
// as a session runs and as more targets/hops accumulate — which is exactly
// why the app got progressively laggier over time instead of staying flat
// like PingPlotter. Jumping straight to `cutoff` via lower_bound turns that
// into O(log N + only the in-window samples), independent of total history.
// std::deque (not vector) gives the SAME O(log N) random-access binary search
// but O(1) pop_front for trimming old samples — see SampleBuf below for why
// that distinction matters for anything left running 24/7.
using SampleBuf = std::deque<Sample>;

inline SampleBuf::const_iterator focus_begin(const SampleBuf& pts, std::optional<double> cutoff) {
    if (!cutoff) return pts.begin();
    return std::lower_bound(pts.begin(), pts.end(), *cutoff,
                             [](const Sample& s, double c) { return s.first < c; });
}

// How many raw samples per hop to retain, given the probe rate. Sized to
// comfortably cover the longest focus window the UI offers (24h), not a flat
// one-size-fits-all number: a slow-probing target (e.g. one sample every 10s)
// needs far fewer than a fast one (e.g. 10/s) to cover the same 24h, so this
// scales storage to what's actually needed instead of over-provisioning
// every target for the fastest case. Clamped to a sane floor/ceiling so a
// misconfigured probe interval can't starve retention or blow up memory.
inline size_t sample_cap_for(double probe_interval) {
    constexpr double kMaxFocusSecs = 86400.0; // "Last 24 hours", the longest UI option
    double interval = probe_interval > 0.01 ? probe_interval : 1.0;
    double needed = (kMaxFocusSecs / interval) * 1.1; // +10% headroom
    return static_cast<size_t>(std::clamp(needed, 2000.0, 100000.0));
}

inline std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': break;
            case '\t': o += "\\t"; break;
            default: o.push_back(c);
        }
    }
    return o;
}

inline std::string numOrNull(std::optional<double> v) {
    if (!v) return "null";
    std::ostringstream os;
    os << *v;
    return os.str();
}

// ---------------------------------------------------------------- session mgmt
struct WebTarget {
    uint64_t id;
    std::string name;
    std::unique_ptr<std::atomic<bool>> stop;
    std::unique_ptr<std::atomic<bool>> paused;
    std::shared_ptr<Session> session; // shared so config can be edited live
    std::thread th;
    std::mutex mtx;
    Snapshot latest;
    bool has = false;
    std::map<uint8_t, SampleBuf> series;
    double added_at = 0; // for the warm-up window — see kWarmupSecs below
    ~WebTarget() {
        if (stop) stop->store(true);
        if (th.joinable()) th.join();
    }
};

// A freshly-added target immediately probes every hop continuously and at
// full rate — a much heavier, more sustained burst of ICMP than a one-shot
// `tracert` ever generates. Some routers/edges rate-limit ICMP generation
// (RFC 1812) and can visibly throttle or queue replies for the first several
// seconds of that burst before settling, producing exactly the "big ramp
// right after adding a target" pattern. That's an external network behavior,
// not something NetPulse can prevent outright — but we don't have to show it:
// samples from the first kWarmupSecs after a target is added are still
// probed (so hop/route discovery isn't delayed) but are excluded from the
// exposed stats and chart series, so the UI only ever shows data from once
// things have settled. This does NOT suppress a similar rate-limit event if
// it recurs LATER in a long-running session — only the one at start.
constexpr double kWarmupSecs = 10.0;

struct Stat {
    double loss = 0;
    std::optional<double> cur, avg, min, max, med;
    double std = 0;
    double jitter = 0;
    size_t sent = 0, recv = 0;
};

inline Stat compute_stat(const SampleBuf& pts, std::optional<double> focus) {
    std::optional<double> cutoff;
    if (focus) cutoff = now_secs() - *focus;
    Stat s;
    std::vector<double> rtts;
    std::optional<double> cur;
    for (auto it = focus_begin(pts, cutoff); it != pts.end(); ++it) {
        const auto& [ts, rtt] = *it;
        ++s.sent;
        cur = rtt;
        if (rtt) rtts.push_back(*rtt);
    }
    s.recv = rtts.size();
    s.loss = s.sent ? (1.0 - double(s.recv) / s.sent) * 100.0 : 0.0;
    s.cur = cur;
    if (!rtts.empty()) {
        double mn = rtts[0], mx = rtts[0], sum = 0;
        for (double r : rtts) { mn = (std::min)(mn, r); mx = (std::max)(mx, r); sum += r; }
        double avg = sum / rtts.size(), var = 0;
        for (double r : rtts) var += (r - avg) * (r - avg);
        var /= rtts.size();
        s.min = mn; s.max = mx; s.avg = avg; s.std = std::sqrt(var);
        // Jitter as the MEDIAN (not mean) of successive absolute differences.
        // With a mean, a single huge outlier contributes two enormous jumps
        // (up, then back down) that get averaged over the WHOLE focus window,
        // so reported jitter stays inflated for as long as that one sample
        // remains in view — exactly the "takes forever to come back down"
        // complaint. The median of the same jump sequence is unmoved by one
        // or two extreme jumps as long as most recent samples are normal, so
        // it reflects current network behavior almost immediately instead of
        // the historical worst moment in the window.
        std::vector<double> diffs;
        for (size_t i = 1; i < rtts.size(); ++i) diffs.push_back(std::abs(rtts[i] - rtts[i - 1]));
        if (!diffs.empty()) {
            std::sort(diffs.begin(), diffs.end());
            size_t dm = diffs.size() / 2;
            s.jitter = (diffs.size() % 2) ? diffs[dm] : (diffs[dm - 1] + diffs[dm]) / 2.0;
        }
        std::vector<double> sorted = rtts;
        std::sort(sorted.begin(), sorted.end());
        size_t m = sorted.size() / 2;
        s.med = (sorted.size() % 2) ? sorted[m] : (sorted[m - 1] + sorted[m]) / 2.0;
    }
    return s;
}

class Manager {
public:
    uint64_t add(const std::string& name, Settings st) {
        auto t = std::make_unique<WebTarget>();
        t->id = nextId_++;
        t->name = name;
        t->added_at = now_secs();
        t->stop = std::make_unique<std::atomic<bool>>(false);
        t->paused = std::make_unique<std::atomic<bool>>(false);
        WebTarget* raw = t.get();
        std::atomic<bool>* stop = t->stop.get();
        std::atomic<bool>* paused = t->paused.get();
        uint64_t id = t->id;
        auto sess = std::make_shared<Session>(id, name, st);
        t->session = sess;
        t->th = std::thread([raw, sess, stop, paused]() {
            // Timing accuracy in this loop depends on the thread actually
            // getting scheduled promptly when a reply arrives — if the OS
            // delays it (e.g. while the Electron main/UI thread is busy), the
            // packet still arrived on time, but we don't read+timestamp it
            // until we resume, so the reported RTT is inflated by however
            // long we were descheduled. Mildly favoring this thread reduces
            // (does not eliminate) that risk; a follow-up using kernel-level
            // receive timestamps (SO_TIMESTAMP / SCM_TIMESTAMPING) would
            // close the gap completely, independent of scheduling.
#if defined(_WIN32)
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#elif defined(__linux__)
            ::setpriority(PRIO_PROCESS, 0, -5); // best-effort; ignored without CAP_SYS_NICE
#endif
            sess->run(stop, paused, [raw, sess](const Snapshot& snap) {
                std::lock_guard<std::mutex> lk(raw->mtx);
                // Right-sized for this target's own probe rate (see
                // sample_cap_for) rather than one flat number for everyone,
                // and trimmed with pop_front — O(1) on a deque, unlike the
                // O(n) vector::erase(begin()) this used to do on EVERY sample
                // once a hop's history filled up. That one-line difference is
                // exactly what made a target left running 24/7 get steadily
                // slower forever after ~55 hours: every single new probe
                // reply was shifting up to 200,000 elements to make room.
                size_t cap = sample_cap_for(sess->settings_snapshot().probe_interval);
                bool warming_up = (now_secs() - raw->added_at) < kWarmupSecs;
                for (const auto& np : snap.new_points) {
                    if (warming_up) continue; // don't expose warm-up-period samples to stats/chart
                    auto& v = raw->series[np.hop];
                    if (v.empty() || v.back().first != np.ts) v.emplace_back(np.ts, np.rtt);
                    while (v.size() > cap) v.pop_front();
                }
                raw->latest = snap;
                raw->has = true;
            });
        });
        std::lock_guard<std::mutex> lk(listMtx_);
        targets_.push_back(std::move(t));
        return id;
    }

    // Live-edit a running target's config (interface, probe interval, timeout,
    // payload, max hops, family). Returns false if the id is unknown.
    bool update(uint64_t id, const Settings& s) {
        std::lock_guard<std::mutex> lk(listMtx_);
        for (auto& t : targets_)
            if (t->id == id && t->session) { t->session->update_settings(s); return true; }
        return false;
    }

    // Current config of a target (for the UI to show what a target is using).
    std::optional<Settings> settings_of(uint64_t id) {
        std::lock_guard<std::mutex> lk(listMtx_);
        for (auto& t : targets_)
            if (t->id == id && t->session) return t->session->settings_snapshot();
        return std::nullopt;
    }

    void pause(uint64_t id, bool on) {
        std::lock_guard<std::mutex> lk(listMtx_);
        for (auto& t : targets_)
            if (t->id == id && t->paused) t->paused->store(on);
    }

    void stop(uint64_t id) {
        std::lock_guard<std::mutex> lk(listMtx_);
        for (auto& t : targets_)
            if (t->id == id && t->stop) t->stop->store(true);
    }

    void remove(uint64_t id) {
        std::lock_guard<std::mutex> lk(listMtx_);
        for (auto it = targets_.begin(); it != targets_.end(); ++it)
            if ((*it)->id == id) { targets_.erase(it); return; }
    }

    std::string state_json(std::optional<double> focus) {
        std::lock_guard<std::mutex> lk(listMtx_);
        std::ostringstream o;
        o << "{\"targets\":[";
        for (size_t k = 0; k < targets_.size(); ++k) {
            auto& t = *targets_[k];
            std::lock_guard<std::mutex> tl(t.mtx);
            o << "{\"id\":" << t.id << ",\"name\":\"" << esc(t.name) << "\",";
            o << "\"paused\":" << ((t.paused && t.paused->load()) ? "true" : "false") << ",";
            {
                double remaining = kWarmupSecs - (now_secs() - t.added_at);
                o << "\"warmup_remaining\":" << (remaining > 0 ? remaining : 0.0) << ",";
            }
            o << "\"dest_ip\":\"" << esc(t.latest.dest_ip ? *t.latest.dest_ip : "") << "\",";
            o << "\"family\":\"" << (t.latest.family ? (*t.latest.family == Family::V4 ? "IPv4" : "IPv6") : "") << "\",";
            o << "\"error\":\"" << esc(t.latest.error ? *t.latest.error : "") << "\",";
            if (t.session) {
                Settings cfg = t.session->settings_snapshot();
                const char* fam = cfg.family == FamilyPref::V4 ? "v4"
                                  : cfg.family == FamilyPref::V6 ? "v6" : "auto";
                o << "\"config\":{\"probe\":" << cfg.probe_interval
                  << ",\"timeout\":" << cfg.timeout
                  << ",\"payload\":" << cfg.payload_size
                  << ",\"maxhops\":" << int(cfg.max_hops)
                  << ",\"raw\":" << (cfg.privileged ? "true" : "false")
                  << ",\"family\":\"" << fam << "\""
                  << ",\"src\":\"" << esc(cfg.source_addr) << "\"},";
            }
            o << "\"hops\":[";
            double cutoff = focus ? now_secs() - *focus : 0;
            for (size_t h = 0; h < t.latest.hops.size(); ++h) {
                const auto& hop = t.latest.hops[h];
                static const SampleBuf kEmpty;
                const SampleBuf& pts = t.series.count(hop.hop) ? t.series[hop.hop] : kEmpty;
                Stat s = compute_stat(pts, focus);
                o << "{\"hop\":" << int(hop.hop)
                  << ",\"address\":\"" << esc(hop.address ? *hop.address : "*") << "\""
                  << ",\"hostname\":\"" << esc(hop.hostname ? *hop.hostname : "") << "\""
                  << ",\"is_dest\":" << (hop.is_dest ? "true" : "false")
                  << ",\"loss\":" << s.loss << ",\"cur\":" << numOrNull(s.cur)
                  << ",\"avg\":" << numOrNull(s.avg) << ",\"min\":" << numOrNull(s.min)
                  << ",\"max\":" << numOrNull(s.max) << ",\"med\":" << numOrNull(s.med)
                  << ",\"std\":" << s.std
                  << ",\"jitter\":" << s.jitter
                  << ",\"sent\":" << s.sent << ",\"recv\":" << s.recv << "}";
                if (h + 1 < t.latest.hops.size()) o << ",";
            }
            o << "],\"series\":{";
            bool firstHop = true;
            // Stable downsampling: bucket samples onto a FIXED absolute-time grid
            // (bucket = floor(ts / width)), keeping the worst (max) RTT per bucket.
            // Because the grid is anchored to wall-clock time (not to the shifting
            // sample index), the same sample always lands in the same bucket, so
            // the line no longer flickers/jumps as new points arrive, and spikes
            // are always preserved instead of aliasing in and out.
            double span = focus ? *focus : 0.0;
            for (auto& [hop, pts] : t.series) {
                if (span <= 0.0) { // "all": span = first..now
                    for (auto& [ts, rtt] : pts) { span = now_secs() - ts; break; }
                }
                size_t cap = 600;
                double width = span > 0 ? span / cap : 1.0;
                if (width <= 0) width = 1.0;
                // bucket -> (representative ts, max rtt or none, sawLoss)
                std::map<long long, std::tuple<double, std::optional<double>, bool>> buckets;
                for (auto it = focus_begin(pts, focus ? std::optional<double>(cutoff) : std::nullopt);
                     it != pts.end(); ++it) {
                    const auto& [ts, rtt] = *it;
                    long long b = static_cast<long long>(ts / width);
                    auto bit = buckets.find(b);
                    if (bit == buckets.end()) {
                        buckets.emplace(b, std::make_tuple(ts, rtt, !rtt.has_value()));
                    } else {
                        auto& [bts, brtt, bloss] = bit->second;
                        if (!rtt.has_value()) bloss = true;
                        else if (!brtt.has_value() || *rtt > *brtt) { brtt = rtt; bts = ts; }
                    }
                }
                if (!firstHop) o << ",";
                firstHop = false;
                o << "\"" << int(hop) << "\":[";
                bool firstPt = true;
                for (auto& [b, tup] : buckets) {
                    auto& [bts, brtt, bloss] = tup;
                    if (!firstPt) o << ",";
                    firstPt = false;
                    o << "[" << std::fixed << bts << ","
                      << (brtt ? std::to_string(*brtt) : "null") << "]";
                }
                o << "]";
            }
            o << "}}";
            if (k + 1 < targets_.size()) o << ",";
        }
        o << "]}";
        return o.str();
    }

private:
    std::vector<std::unique_ptr<WebTarget>> targets_;
    std::mutex listMtx_;
    uint64_t nextId_ = 1;
};

} // namespace netpulse

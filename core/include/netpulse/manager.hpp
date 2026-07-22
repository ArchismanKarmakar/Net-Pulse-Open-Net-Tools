// NetPulse target Manager — the shared engine + per-target state/series + JSON
// state production. Used by BOTH the HTTP server (server/main.cpp) and the
// Node-API addon (napi/napi.cpp), so they drive the identical verified core.
#pragma once

#include "netpulse/session.hpp"
#include "netpulse/stats.hpp"
#include "netpulse/transport.hpp"
#include "netpulse/ping_run.hpp"

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

// WebTarget::series used to retain up to 100,000 raw samples PER HOP (see
// the now-removed sample_cap_for(), sized to cover a 24h focus window at the
// fastest supported probe rate) — fine for a handful of targets, but at
// 100+ long-running targets x ~30 hops each this is the dominant RAM cost in
// the whole process, and it's what's actually serialized into the live JSON
// on every poll. series is now bounded by TIME instead (kHotWindowSecs, same
// constant HopStats' own hot tier uses — see stats.hpp), and whatever ages
// out has already been durably flushed to disk by HopStats::push() (which
// runs on the session thread, ahead of series' own aging here) — see
// ColdStore in stats.hpp/stats.cpp, keyed by the same (target_id, hop) this
// struct uses. So aging here just drops the RAM copy; nothing is lost, and
// nothing gets double-written to the cold file that HopStats already owns.
// A focus window (or "all") reaching further back than the hot tier is
// served by merging in ColdStore::aggregate()/read_range() — see
// compute_stat_tiered() and the series/chart loop in state_json() below.

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

// RFC 4180 CSV field escaping — deliberately separate from esc() above
// (which is JSON-string escaping, used everywhere state_json() builds JSON
// text). A field is only quoted if it actually needs it (contains a comma,
// quote, or newline); internal quotes are doubled per the RFC. Used by
// export_target_full_csv/export_all_targets_full_csv below — a target name
// or hostname containing a comma would otherwise pass through with the
// comma unescaped, silently shifting every column after it in the exported
// file (and in turn breaking anything, like an XLSX importer, that parses
// that CSV expecting well-formed rows).
inline std::string csv_esc(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\"\"";
        else o.push_back(c);
    }
    o += "\"";
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
    std::map<uint8_t, int> discovery_count;            // per-hop, see kDiscoveryDropCount below
    std::map<uint8_t, double> discovery_first_reply_at; // time of the first real reply for each hop
    std::map<uint8_t, double> discovery_first_seen_at;  // time of the first sample (reply OR loss) for each hop
    std::optional<uint8_t> dest_reset_for;              // dest hop whose discovery-burst samples we've already discarded
    ~WebTarget() {
        if (stop) stop->store(true);
        if (th.joinable()) th.join();
    }
};

// A hop's very first replies right after it's discovered can be genuinely
// unrepresentative (first-hit route/ARP/conntrack setup, or the tail of
// whatever briefly rate-limited the burst that found it) rather than steady
// -state behavior — this is what produced a hop showing SENT=1 with that one
// sample already at 15000+ms. A single global "wait N seconds since the
// target was added" timer can't fix this correctly, because hops are
// discovered at different real times: an early hop might have a dozen clean
// samples by the time a slow/lossy hop gets its very first reply, so a
// target-wide timer either cuts off too early for late hops or unnecessarily
// delays early ones. Instead, this is gated PER HOP, by how many raw replies
// THAT hop has actually received: its first kDiscoveryDropCount replies are
// used only to establish its address/route (already visible immediately —
// see `t.latest.hops` below, populated regardless) and are not written into
// the exposed stats/series; real numbers for that hop start from its own
// (kDiscoveryDropCount+1)-th reply, whenever that happens to occur.
// If the probe interval is long, waiting for kDiscoveryDropCount replies can
// otherwise leave the UI without any stats for many seconds. In that case we
// stop suppressing after a short grace window so the chart still begins
// showing data promptly.
//
// The SAME settling window is applied to a hop's early LOSSES, not just its
// replies. This is what fixes the "IPv6 hostname target starts at 100% loss
// for 3–5 s, then recovers" report: IPv6 paths (e.g. Cloudflare / BSNL) tend to
// rate-limit the initial probe burst that discovers the route, so a hop's first
// couple of probes are dropped by the network. If those losses were recorded
// immediately while the hop's genuine early replies are still being held back
// for settling, the hop reads as 100% loss until the reply-drop clears — which
// is exactly why IPv6 flashed loss at startup but IPv4 (whose initial probes
// weren't dropped) did not. By holding a hop's early losses for the same
// per-hop window, a hop stays blank ("discovering") until it has either
// produced kDiscoveryDropCount real replies or kDiscoveryDropMaxSecs has
// elapsed since it was first seen — after which losses record normally, so a
// genuinely unreachable hop still surfaces its 100% loss a few seconds in.
constexpr int kDiscoveryDropCount = 3;
constexpr double kDiscoveryDropMaxSecs = 5.0;

// Debounce for the derived "up/down" reading in state_json()'s uptime chip:
// the destination hop must show zero replies for this long before the UI
// reports "down" — a single lost probe (normal jitter, not an outage)
// shouldn't flip the chip. Same spirit as kDiscoveryDropMaxSecs above, just
// for the opposite edge (losing reachability rather than gaining a settled
// reading). No state is tracked between polls — see state_json()'s uptime
// block, which just scans the tail of the destination hop's own samples.
constexpr double kUptimeDownGraceSecs = 5.0;
// Window the derived uptime chip's "upPct" figure is computed over — a long,
// fixed window independent of the user's focus-window toggle, so the chip
// answers "how reliable has this target been", not "how reliable right
// now" (that's what the focus-window stat/chart already show).
constexpr double kUptimeStatsWindowSecs = 86400.0;

struct Stat {
    double loss = 0;
    std::optional<double> cur, avg, min, max, med;
    double std = 0;
    double jitter = 0;
    size_t sent = 0, recv = 0;
};

// target_id/hop identify which ColdStore file to merge in for whatever part
// of `focus` reaches further back than `pts` (the RAM hot tier, bounded to
// kHotWindowSecs — see WebTarget::series) actually holds. med/jitter are
// deliberately NOT extended into the cold tier: both need the individual
// sample sequence (median, successive-difference), which the cold aggregate
// doesn't retain (see ColdAggregate in stats.hpp) — they stay "recent-window
// precision" figures over just the hot tier, same tradeoff most APM tools
// make for long lookback windows. sent/recv/loss/min/max/avg/std all merge
// cleanly from summary statistics alone, so those do extend across the full
// requested focus window.
inline Stat compute_stat(const SampleBuf& pts, std::optional<double> focus, uint64_t target_id = 0,
                          uint8_t hop = 0) {
    std::optional<double> cutoff;
    if (focus) cutoff = now_secs() - *focus;
    Stat s;
    std::vector<double> rtts;
    std::optional<double> cur;
    size_t hot_sent = 0;
    for (auto it = focus_begin(pts, cutoff); it != pts.end(); ++it) {
        const auto& [ts, rtt] = *it;
        ++hot_sent;
        cur = rtt;
        if (rtt) rtts.push_back(*rtt);
    }
    bool need_cold = !cutoff || pts.empty() || *cutoff < pts.front().first;
    ColdAggregate cold;
    if (need_cold) cold = ColdStore::instance().aggregate(target_id, hop, cutoff);
    s.sent = hot_sent + cold.sent;
    s.recv = rtts.size() + cold.recv;
    s.loss = s.sent ? (1.0 - double(s.recv) / s.sent) * 100.0 : 0.0;
    s.cur = cur;
    bool hot_has = !rtts.empty();
    if (hot_has || cold.recv > 0) {
        double hot_sum = 0, hot_sumsq = 0, hot_min = 0, hot_max = 0;
        if (hot_has) {
            hot_min = rtts[0]; hot_max = rtts[0];
            for (double r : rtts) { hot_min = (std::min)(hot_min, r); hot_max = (std::max)(hot_max, r); hot_sum += r; hot_sumsq += r * r; }
        }
        double mn = hot_has ? hot_min : cold.minv;
        double mx = hot_has ? hot_max : cold.maxv;
        if (hot_has && cold.recv > 0) { mn = (std::min)(mn, cold.minv); mx = (std::max)(mx, cold.maxv); }
        double sum = hot_sum + cold.sum, sumsq = hot_sumsq + cold.sumsq;
        double avg = sum / s.recv;
        double var = sumsq / s.recv - avg * avg;
        s.min = mn; s.max = mx; s.avg = avg; s.std = std::sqrt((std::max)(0.0, var));
    }
    if (!rtts.empty()) {
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

// ---------------------------------------------------------------------------
// Network-interface-change watchdog — the primary fix for "sleep/wake leaves
// every target stuck." A sleep/resume cycle (or any interface flap — Wi-Fi
// roam, VPN toggle, cable unplug) reliably changes what list_interfaces()
// (transport.hpp/cpp, already cross-platform) reports: an address disappears
// and/or a new one appears, even if it's ultimately the "same" network. That
// makes it a much faster and more reliable signal than waiting for any
// individual session to notice its own socket/DNS has gone stale — this
// reacts the moment the OS reports the change, for every target at once, by
// asking each session to rebuild (Session::request_rebuild(), session.hpp),
// which reuses the exact resolve()+acquire_sock() path settings edits
// already exercise. Session::run() also has its own independent fallback
// self-heal (consecutive send failures / total silence — see session.cpp)
// for the case this watchdog can't catch: the interface list looking
// unchanged while the route underneath it still isn't actually usable yet.
//
// Same "process-lifetime leaked singleton with a detached worker thread" 
// shape as RdnsResolver/RxDispatcher in session.cpp — Manager itself is
// used as a single long-lived instance for the whole process (HTTP server /
// NAPI addon), so there's no clean shutdown point to join a thread against,
// same as those.
namespace {
constexpr double kNetworkWatchdogPollSecs = 3.0;

struct NetworkWatchdog {
    std::mutex mtx;
    bool started = false;
    std::set<std::string> last_snapshot; // "name|address" per interface
    bool have_snapshot = false;          // false until the first poll — first poll only establishes a baseline, never triggers

    static std::set<std::string> snapshot() {
        std::set<std::string> out;
        for (const auto& ni : list_interfaces()) out.insert(ni.name + "|" + ni.address);
        return out;
    }

    // `on_change` is called (from the watchdog's own thread) whenever the
    // interface set differs from the last poll. Manager::add() passes a
    // closure that walks targets_ under listMtx_ and calls
    // session->request_rebuild() on each — kept as a callback here rather
    // than this struct reaching into Manager directly, so this watchdog
    // stays a small, independently-reasoned-about piece exactly like
    // RdnsResolver/RxDispatcher are.
    void ensure_started(std::function<void()> on_change) {
        std::lock_guard<std::mutex> lk(mtx);
        if (started) return;
        started = true;
        std::thread([this, on_change = std::move(on_change)]() {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::duration<double>(kNetworkWatchdogPollSecs));
                auto snap = snapshot();
                bool changed;
                {
                    std::lock_guard<std::mutex> lk2(mtx);
                    changed = have_snapshot && snap != last_snapshot;
                    last_snapshot = snap;
                    have_snapshot = true;
                }
                if (changed) on_change();
            }
        }).detach(); // process-lifetime; no join needed — see class comment above
    }
};
inline NetworkWatchdog& g_network_watchdog() { static NetworkWatchdog* w = new NetworkWatchdog(); return *w; } // leaked singleton, same pattern as g_rdns()/g_rx() in session.cpp
} // namespace

// ---------------------------------------------------------------------------
// Standalone Ping tool run — see ping_run.hpp's doc comment for why this
// exists as a real engine object instead of an OS `ping` subprocess.
// Deliberately separate from WebTarget/targets_ below: a ping run is short-
// lived and one-shot-oriented (even "continuous" mode is a single ad-hoc
// diagnostic a user starts and stops, not a standing monitored target), so
// it's tracked in its own list with its own lifecycle — auto-removed once
// finished AND fully drained, rather than living until the user explicitly
// removes it the way a target does.
struct PingRunHandle {
    uint64_t id;
    std::string cmd_display; // human-readable summary shown in the UI in place of a fake shell command line
    std::unique_ptr<std::atomic<bool>> stop;
    std::shared_ptr<PingRun> run_obj;
    std::thread th;
    std::mutex mtx;
    std::vector<PingLine> lines; // append-only; poll_ping() below drains from `drained` onward
    size_t drained = 0;
    bool done = false;
    ~PingRunHandle() {
        if (stop) stop->store(true);
        if (th.joinable()) th.join();
    }
};

class Manager {
public:
    uint64_t add(const std::string& name, Settings st) {
        g_network_watchdog().ensure_started([this]() {
            std::lock_guard<std::mutex> lk(listMtx_);
            for (auto& t : targets_)
                if (t->session) t->session->request_rebuild();
        });
        auto t = std::make_unique<WebTarget>();
        t->id = nextId_++;
        t->name = name;
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
                for (const auto& np : snap.new_points) {
                    // First time we see ANY sample (reply or loss) for this hop,
                    // record when — this bounds the per-hop settling window for
                    // losses too (see the note above kDiscoveryDropCount).
                    if (!raw->discovery_first_seen_at.count(np.hop))
                        raw->discovery_first_seen_at[np.hop] = np.ts;
                    double seen_elapsed = np.ts - raw->discovery_first_seen_at[np.hop];

                    if (np.rtt.has_value()) {
                        // Real reply: hold this hop's first kDiscoveryDropCount
                        // replies for settling (unrepresentative first-hit RTTs).
                        int& n = raw->discovery_count[np.hop];
                        if (n == 0) raw->discovery_first_reply_at[np.hop] = np.ts;
                        ++n;
                        double elapsed = np.ts - raw->discovery_first_reply_at[np.hop];
                        if (n <= kDiscoveryDropCount && elapsed < kDiscoveryDropMaxSecs) continue; // still settling this hop — don't expose yet
                    } else {
                        // Loss/timeout: hold a hop's EARLY losses for the same
                        // settling window so a rate-limited initial burst (the
                        // IPv6 startup case) doesn't read as 100% loss before the
                        // hop has had a fair chance to answer. Once the hop has
                        // produced kDiscoveryDropCount real replies, OR the grace
                        // window has elapsed since it was first seen, losses
                        // record normally — so a genuinely unreachable hop still
                        // surfaces its 100% loss shortly after.
                        int replies = raw->discovery_count.count(np.hop) ? raw->discovery_count[np.hop] : 0;
                        if (replies < kDiscoveryDropCount && seen_elapsed < kDiscoveryDropMaxSecs) continue; // still settling — suppress early loss
                    }
                    auto& v = raw->series[np.hop];
                    if (v.empty() || v.back().first != np.ts) v.emplace_back(np.ts, np.rtt);
                    // Bounded by time (kHotWindowSecs), matching HopStats'
                    // own hot tier — the sample being dropped here has
                    // already been durably flushed to the cold file by
                    // HopStats::push() (same session, same np.ts, run just
                    // ahead of this on_update callback), so this is purely a
                    // RAM trim, not a data loss.
                    double cutoff = np.ts - kHotWindowSecs;
                    while (!v.empty() && v.front().first < cutoff) v.pop_front();
                }

                // Root-cause fix for the "IPv6 target starts at 100% loss, then
                // recovers" report. While the route is still being discovered,
                // the engine fans probes out across many TTLs at once, and EVERY
                // TTL at or beyond the destination's distance reaches the
                // destination itself — so the destination is hit by a burst of
                // echo requests during discovery. IPv6 anycast destinations
                // (Cloudflare/Google) rate-limit ICMPv6 echo far more
                // aggressively than their IPv4 counterparts, so that burst is
                // partially dropped and the destination hop's FIRST samples are
                // burst-induced losses rather than steady state. IPv4 dests
                // (e.g. 1.1.1.1) absorb the same burst without dropping, which is
                // exactly why IPv4 never showed the startup loss and IPv6 did.
                // The moment the destination is confirmed (dest_hop known), the
                // fan-out stops (max_hop collapses to the dest) and probing drops
                // to one packet per interval — steady state. So we discard the
                // destination hop's discovery-phase samples once, letting it
                // measure fresh from that point. Family-agnostic, but only IPv6
                // actually had burst losses to discard.
                if (snap.dest_hop && raw->dest_reset_for != snap.dest_hop) {
                    uint8_t dh = *snap.dest_hop;
                    raw->series.erase(dh);
                    raw->discovery_count.erase(dh);
                    raw->discovery_first_seen_at.erase(dh);
                    raw->discovery_first_reply_at.erase(dh);
                    raw->dest_reset_for = snap.dest_hop;
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

    // Just one target's "config" object, same field shape as state_json()'s
    // per-target config block — for a host to read back a single target's
    // current settings (e.g. to merge a partial update) WITHOUT building and
    // parsing the entire multi-target state payload. Empty object if the id
    // is unknown.
    std::string target_config_json(uint64_t id) {
        auto s = settings_of(id);
        if (!s) return "{}";
        const Settings& cfg = *s;
        const char* fam = cfg.family == FamilyPref::V4 ? "v4"
                          : cfg.family == FamilyPref::V6 ? "v6" : "auto";
        std::ostringstream o;
        o << "{\"probe\":" << cfg.probe_interval
          << ",\"trace\":" << cfg.trace_interval
          << ",\"timeout\":" << cfg.timeout
          << ",\"payload\":" << cfg.payload_size
          << ",\"maxhops\":" << int(cfg.max_hops)
          << ",\"raw\":" << (cfg.privileged ? "true" : "false")
          << ",\"family\":\"" << fam << "\""
          << ",\"src\":\"" << esc(cfg.source_addr) << "\""
          << ",\"pausedHops\":[";
        { bool first = true; for (uint8_t ph : cfg.paused_hops) { if (!first) o << ","; o << int(ph); first = false; } }
        o << "]}";
        return o.str();
    }

    // Manually re-trigger route discovery for a target without touching its
    // family/source/socket — see Session::force_recheck() for the full
    // rationale. No-op if the id is unknown.
    void force_recheck(uint64_t id) {
        std::lock_guard<std::mutex> lk(listMtx_);
        for (auto& t : targets_)
            if (t->id == id && t->session) { t->session->force_recheck(); return; }
    }

    // Snapshot of every currently-known target id — used by hosts that walk
    // all targets for their own bookkeeping without needing to round-trip
    // through state_json().
    std::vector<uint64_t> target_ids() {
        std::lock_guard<std::mutex> lk(listMtx_);
        std::vector<uint64_t> ids;
        ids.reserve(targets_.size());
        for (auto& t : targets_) ids.push_back(t->id);
        return ids;
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
            o << "\"dest_ip\":\"" << esc(t.latest.dest_ip ? *t.latest.dest_ip : "") << "\",";
            o << "\"family\":\"" << (t.latest.family ? (*t.latest.family == Family::V4 ? "IPv4" : "IPv6") : "") << "\",";
            o << "\"error\":\"" << esc(t.latest.error ? *t.latest.error : "") << "\",";
            // Separate from `error` on purpose — see the Snapshot doc
            // comments in session.hpp. loopHop/loopDupAt are numbers (or
            // null), not embedded in the text, so the UI can highlight the
            // exact two rows without parsing the message.
            o << "\"loopWarning\":\"" << esc(t.latest.loop_warning ? *t.latest.loop_warning : "") << "\",";
            o << "\"loopHop\":" << (t.latest.loop_hop ? std::to_string(int(*t.latest.loop_hop)) : "null") << ",";
            o << "\"loopDupAt\":" << (t.latest.loop_dup_at ? std::to_string(int(*t.latest.loop_dup_at)) : "null") << ",";
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
                  << ",\"src\":\"" << esc(cfg.source_addr) << "\""
                  << ",\"pausedHops\":[";
                { bool first = true; for (uint8_t ph : cfg.paused_hops) { if (!first) o << ","; o << int(ph); first = false; } }
                o << "]},";
            }
            o << "\"hops\":[";
            double cutoff = focus ? now_secs() - *focus : 0;
            for (size_t h = 0; h < t.latest.hops.size(); ++h) {
                const auto& hop = t.latest.hops[h];
                static const SampleBuf kEmpty;
                const SampleBuf& pts = t.series.count(hop.hop) ? t.series[hop.hop] : kEmpty;
                Stat s = compute_stat(pts, focus, t.id, hop.hop);
                o << "{\"hop\":" << int(hop.hop)
                  << ",\"address\":\"" << esc(hop.address ? *hop.address : "*") << "\""
                  << ",\"hostname\":\"" << esc(hop.hostname ? *hop.hostname : "") << "\""
                  << ",\"stale_address\":\"" << esc(hop.stale_address ? *hop.stale_address : "") << "\""
                  << ",\"stale_hostname\":\"" << esc(hop.stale_hostname ? *hop.stale_hostname : "") << "\""
                  << ",\"stale_since\":" << numOrNull(hop.stale_since)
                  << ",\"is_dest\":" << (hop.is_dest ? "true" : "false")
                  << ",\"loss\":" << s.loss << ",\"cur\":" << numOrNull(s.cur)
                  << ",\"avg\":" << numOrNull(s.avg) << ",\"min\":" << numOrNull(s.min)
                  << ",\"max\":" << numOrNull(s.max) << ",\"med\":" << numOrNull(s.med)
                  << ",\"std\":" << s.std
                  << ",\"jitter\":" << s.jitter
                  << ",\"sent\":" << s.sent << ",\"recv\":" << s.recv;
                o << ",\"mpls\":[";
                for (size_t li = 0; li < hop.mpls_labels.size(); ++li) {
                    uint32_t v = hop.mpls_labels[li];
                    if (li) o << ",";
                    o << "{\"label\":" << (v >> 12)
                      << ",\"exp\":" << ((v >> 9) & 0x7)
                      << ",\"ttl\":" << (v & 0xff) << "}";
                }
                o << "]}";
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
                // A focus window (or "all") reaching further back than the
                // RAM hot tier holds needs the cold-tier's older points too —
                // otherwise the chart line would visibly stop at
                // kHotWindowSecs even though the hop stat above already
                // reports loss/avg/etc. over the full requested window.
                std::optional<double> foc_cutoff = focus ? std::optional<double>(cutoff) : std::nullopt;
                bool need_cold = !foc_cutoff || pts.empty() || *foc_cutoff < pts.front().first;
                std::vector<Sample> cold_pts;
                if (need_cold) cold_pts = ColdStore::instance().read_range(t.id, hop, foc_cutoff);
                if (span <= 0.0) { // "all": span = first..now
                    if (!cold_pts.empty()) span = now_secs() - cold_pts.front().first;
                    else for (auto& [ts, rtt] : pts) { span = now_secs() - ts; break; }
                }
                size_t cap = 600;
                double width = span > 0 ? span / cap : 1.0;
                if (width <= 0) width = 1.0;
                // bucket -> (representative ts, max rtt or none, sawLoss)
                std::map<long long, std::tuple<double, std::optional<double>, bool>> buckets;
                auto add_point = [&](double ts, std::optional<double> rtt) {
                    long long b = static_cast<long long>(ts / width);
                    auto bit = buckets.find(b);
                    if (bit == buckets.end()) {
                        buckets.emplace(b, std::make_tuple(ts, rtt, !rtt.has_value()));
                    } else {
                        auto& [bts, brtt, bloss] = bit->second;
                        if (!rtt.has_value()) bloss = true;
                        else if (!brtt.has_value() || *rtt > *brtt) { brtt = rtt; bts = ts; }
                    }
                };
                for (const auto& [ts, rtt] : cold_pts) add_point(ts, rtt);
                for (auto it = focus_begin(pts, foc_cutoff); it != pts.end(); ++it) {
                    const auto& [ts, rtt] = *it;
                    add_point(ts, rtt);
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
            o << "},";
            // Derived uptime chip: no bespoke tracking state, no event log —
            // purely a query over the destination hop's own sample history
            // (see kUptimeStatsWindowSecs / kUptimeDownGraceSecs above).
            // `up` is a cheap tail scan of just the destination hop's most
            // recent samples; `pct` reuses compute_stat's loss figure
            // (already computed for the hop stat above) inverted, over a
            // long fixed window independent of the user's focus toggle.
            {
                bool up = true;
                double pct = 100.0;
                if (t.latest.dest_hop) {
                    uint8_t dh = *t.latest.dest_hop;
                    static const SampleBuf kEmptyDest;
                    const SampleBuf& dpts = t.series.count(dh) ? t.series[dh] : kEmptyDest;
                    double now = now_secs();
                    for (auto it = dpts.rbegin(); it != dpts.rend(); ++it) {
                        if (now - it->first > kUptimeDownGraceSecs) break;
                        if (it->second.has_value()) { up = true; break; }
                        up = false;
                    }
                    Stat ds = compute_stat(dpts, kUptimeStatsWindowSecs, t.id, dh);
                    if (ds.sent > 0) pct = 100.0 - ds.loss;
                }
                o << "\"uptime\":{\"up\":" << (up ? "true" : "false")
                  << ",\"pct\":" << pct << "}";
            }
            o << "}"; // close the target object opened at the top of this iteration
            if (k + 1 < targets_.size()) o << ",";
        }
        o << "]}";
        return o.str();
    }

    // ---------------------------------------------------------------------
    // Full-fidelity CSV export — deliberately NOT built from the same
    // `series` data state_json() sends the chart: that's downsampled to a
    // fixed 600 buckets per hop (see the bucketing loop above) so the UI
    // stays fast regardless of how long a target has run, which is exactly
    // right for a chart and exactly wrong for "export everything." This
    // reads ColdStore directly with no cutoff, which is every raw sample
    // this target has ever recorded for that hop (see refresh() in
    // stats.cpp: cutoff=nullopt starts from record 0 as opposed to a
    // binary-searched offset), merged with whatever's still only in the RAM
    // hot tier and hasn't been flushed to the cold file yet.
    //
    // One row per (hop, sample) — long-format, not one-row-per-hop — because
    // that's what "trend/past data/everything" actually requires: the point
    // list itself, not just its current summary statistics (which the
    // ordinary hop table / hops CSV already cover). A leading `#` comment
    // block carries the target-level context (name, dest, export time) once
    // rather than repeating it on every row.
    static void append_target_rows(std::ostringstream& o, WebTarget& t) {
        for (const auto& hop : t.latest.hops) {
            std::vector<Sample> merged = ColdStore::instance().read_range(t.id, hop.hop, std::nullopt);
            double last_cold_ts = merged.empty() ? -1.0 : merged.back().first;
            auto sit = t.series.find(hop.hop);
            if (sit != t.series.end()) {
                for (const auto& s : sit->second) {
                    // The hot tier's tail can briefly overlap the cold file
                    // (cold flushing runs slightly behind hot pushes) —
                    // only append what's strictly newer than the last cold
                    // record so a sample is never duplicated in the export.
                    if (s.first > last_cold_ts) merged.push_back(s);
                }
            }
            for (const auto& [ts, rtt] : merged) {
                o << t.id << "," << csv_esc(t.name) << "," << csv_esc(t.latest.dest_ip ? *t.latest.dest_ip : "") << ","
                  << int(hop.hop) << "," << csv_esc(hop.address ? *hop.address : "") << ","
                  << csv_esc(hop.hostname ? *hop.hostname : "") << "," << (hop.is_dest ? "true" : "false") << ","
                  << std::fixed << ts << "," << (rtt ? std::to_string(*rtt) : "") << "\n";
            }
        }
    }

    // Every raw sample ever recorded for ONE target, across every hop it has
    // ever had. Empty string if the id is unknown (caller treats that as
    // "nothing to export" — same convention as target_config_json()).
    std::string export_target_full_csv(uint64_t id) {
        std::lock_guard<std::mutex> lk(listMtx_);
        for (auto& tp : targets_) {
            if (tp->id != id) continue;
            std::lock_guard<std::mutex> tl(tp->mtx);
            std::ostringstream o;
            o << "# NetPulse full export - target: " << csv_esc(tp->name) << "\n";
            o << "# dest_ip: " << (tp->latest.dest_ip ? *tp->latest.dest_ip : "") << "\n";
            o << "# exported_at_unix: " << std::fixed << now_secs() << "\n";
            o << "target_id,target_name,dest_ip,hop,address,hostname,is_dest,timestamp_unix,rtt_ms\n";
            append_target_rows(o, *tp);
            return o.str();
        }
        return "";
    }

    // Same shape, every target at once (the target_id/target_name columns
    // are what let this be pivoted back apart in a spreadsheet or pandas).
    std::string export_all_targets_full_csv() {
        std::lock_guard<std::mutex> lk(listMtx_);
        std::ostringstream o;
        o << "# NetPulse full export - ALL targets\n";
        o << "# exported_at_unix: " << std::fixed << now_secs() << "\n";
        o << "target_id,target_name,dest_ip,hop,address,hostname,is_dest,timestamp_unix,rtt_ms\n";
        for (auto& tp : targets_) {
            std::lock_guard<std::mutex> tl(tp->mtx);
            append_target_rows(o, *tp);
        }
        return o.str();
    }

    // ---------------------------------------------------------------------
    // Standalone Ping tool — see PingRunHandle's doc comment above for why
    // this is a separate list from targets_. Poll-based (poll_ping below),
    // not callback/event-based, on purpose: it's the SAME shape get_state()
    // already uses for the main dashboard (the FFI/Rust layer polls this at
    // a tight interval and re-emits it as Tauri events — see commands.rs —
    // so the frontend's existing event-based contract is preserved without
    // this class needing to know anything about Tauri or cxx callbacks).
    uint64_t start_ping(const PingConfig& cfg, std::string cmd_display) {
        auto h = std::make_unique<PingRunHandle>();
        uint64_t id = nextPingId_++;
        h->id = id;
        h->cmd_display = std::move(cmd_display);
        h->stop = std::make_unique<std::atomic<bool>>(false);
        PingRunHandle* raw = h.get();
        std::atomic<bool>* stop = h->stop.get();
        auto run_obj = std::make_shared<PingRun>(id, cfg);
        h->run_obj = run_obj;
        h->th = std::thread([raw, run_obj, stop]() {
            run_obj->run(
                stop,
                [raw](const PingLine& line) {
                    std::lock_guard<std::mutex> lk(raw->mtx);
                    raw->lines.push_back(line);
                },
                [raw]() {
                    std::lock_guard<std::mutex> lk(raw->mtx);
                    raw->done = true;
                });
        });
        std::lock_guard<std::mutex> lk(pingListMtx_);
        ping_runs_.push_back(std::move(h));
        return id;
    }

    void stop_ping(uint64_t id) {
        std::lock_guard<std::mutex> lk(pingListMtx_);
        for (auto& h : ping_runs_)
            if (h->id == id && h->stop) { h->stop->store(true); return; }
    }

    // Returns every line produced since the LAST poll_ping(id) call, plus
    // whether the run has finished — `{"lines":[...],"done":bool}`. Once a
    // run is both done AND every line has been drained by some caller, its
    // handle is erased here (which joins its thread, already a no-op by
    // then since run() already returned) — nothing else ever needs to
    // explicitly "remove" a finished ping the way removing a target does.
    // `{"lines":[],"done":true,"gone":true}` for an id that's already been
    // cleaned up (or never existed) — the frontend treats that identically
    // to a normal "done".
    std::string poll_ping(uint64_t id) {
        std::lock_guard<std::mutex> lk(pingListMtx_);
        for (auto it = ping_runs_.begin(); it != ping_runs_.end(); ++it) {
            PingRunHandle& h = **it;
            if (h.id != id) continue;
            std::ostringstream o;
            bool remove;
            {
                std::lock_guard<std::mutex> tl(h.mtx);
                o << "{\"lines\":[";
                for (size_t i = h.drained; i < h.lines.size(); ++i) {
                    const PingLine& l = h.lines[i];
                    if (i > h.drained) o << ",";
                    o << "{\"seq\":" << l.seq << ",\"ok\":" << (l.ok ? "true" : "false")
                      << ",\"rtt_ms\":" << (l.rtt_ms ? std::to_string(*l.rtt_ms) : "null")
                      << ",\"from\":\"" << esc(l.from_ip) << "\""
                      << ",\"note\":\"" << esc(l.note) << "\"}";
                }
                h.drained = h.lines.size();
                o << "],\"done\":" << (h.done ? "true" : "false") << "}";
                remove = h.done && h.drained >= h.lines.size();
            }
            std::string result = o.str();
            if (remove) ping_runs_.erase(it);
            return result;
        }
        return "{\"lines\":[],\"done\":true,\"gone\":true}";
    }

private:
    std::vector<std::unique_ptr<WebTarget>> targets_;
    std::mutex listMtx_;
    uint64_t nextId_ = 1;
    std::vector<std::unique_ptr<PingRunHandle>> ping_runs_;
    std::mutex pingListMtx_;
    uint64_t nextPingId_ = 1;
};

} // namespace netpulse
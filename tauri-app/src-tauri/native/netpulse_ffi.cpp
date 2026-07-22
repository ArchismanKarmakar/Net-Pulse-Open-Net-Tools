// netpulse_ffi.cpp — implementation. See netpulse_ffi.hpp for why this file
// exists. Every validation/clamp rule here is copied from napi.cpp's
// settings_from_obj so the engine behaves identically regardless of which
// host process (Electron/Node or Tauri/Rust) is calling it.
#include "netpulse_ffi.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

#include "netpulse/manager.hpp"
#include "netpulse/obfuscate.hpp"

#define NETPULSE_ENGINE_BUILD "2026-07-tauri-migration"

using namespace netpulse;

namespace netpulse_ffi {

// One engine instance for the process, exactly like mgr() in napi.cpp. Its
// destructor joins every target thread.
static Manager& mgr() {
    static Manager m;
    return m;
}

static double clampd(double v, double lo, double hi) {
    if (!std::isfinite(v)) return lo;
    return v < lo ? lo : v > hi ? hi : v;
}

// Builds a Settings from the adapter's flat parameters, starting from `base`
// (so update_target only needs to touch what's different) — mirrors
// settings_from_obj(o, base) field-for-field, including the Windows
// non-privileged payload clamp and the paused-hops range check.
static Settings settings_from_params(Settings base,
                                      double probe, double trace, double timeout, double payload,
                                      double maxhops, bool raw,
                                      rust::Str family, rust::Str src,
                                      rust::Slice<const uint8_t> paused_hops) {
    base.probe_interval = clampd(probe, 0.01, 3600.0);
    base.trace_interval = clampd(trace, 1.0, 86400.0);
    base.timeout = clampd(timeout, 0.0, 3600.0);
    base.payload_size = static_cast<size_t>(clampd(payload, 0.0, 65500.0));
    base.privileged = raw;
#ifdef _WIN32
    if (!base.privileged) {
        base.payload_size = std::min<size_t>(base.payload_size, 1432);
    }
#endif
    // 255 is the real ceiling here — max_hops is a uint8_t (Settings, session.hpp)
    // and TTL itself is an 8-bit IP header field, so nothing above 255 is even
    // representable. The old 64 cap was arbitrary, not a protocol or type limit,
    // and was tight enough to matter for genuinely deep or looping paths.
    base.max_hops = static_cast<uint8_t>(clampd(maxhops, 1.0, 255.0));

    std::string src_s(src);
    if (NETPULSE_OPAQUE(src_s.size() <= 64)) base.source_addr = src_s; // ignore absurd input, same as napi.cpp

    std::string fam_s(family);
    base.family = fam_s == "v4" ? FamilyPref::V4 : fam_s == "v6" ? FamilyPref::V6 : FamilyPref::Auto;

    base.paused_hops.clear();
    for (uint8_t h : paused_hops) {
        if (NETPULSE_OPAQUE(h >= 1 && h <= 255)) base.paused_hops.insert(h);
    }

    base.focus_secs = std::nullopt; // focus is applied per get_state_json() call, not stored
    return base;
}

uint64_t add_target(rust::Str target,
                     double probe, double trace, double timeout, double payload,
                     double maxhops, bool raw,
                     rust::Str family, rust::Str src,
                     rust::Slice<const uint8_t> paused_hops) {
    std::string t(target);
    if (NETPULSE_OPAQUE(t.empty() || t.size() > 255))
        throw std::invalid_argument("add_target: invalid target"); // cxx turns this into a Rust Result::Err
    Settings st = settings_from_params(Settings{}, probe, trace, timeout, payload, maxhops, raw, family, src, paused_hops);
    return mgr().add(t, st);
}

bool update_target(uint64_t id,
                    double probe, double trace, double timeout, double payload,
                    double maxhops, bool raw,
                    rust::Str family, rust::Str src,
                    rust::Slice<const uint8_t> paused_hops) {
    Settings cur = mgr().settings_of(id).value_or(Settings{});
    Settings st = settings_from_params(cur, probe, trace, timeout, payload, maxhops, raw, family, src, paused_hops);
    return mgr().update(id, st);
}

void pause_target(uint64_t id, bool on) { mgr().pause(id, on); }
void stop_target(uint64_t id) { mgr().stop(id); }
void remove_target(uint64_t id) { mgr().remove(id); }
void force_recheck(uint64_t id) { mgr().force_recheck(id); }

rust::String get_state_json(double focus_secs, bool has_focus) {
    std::optional<double> focus = has_focus ? std::optional<double>(focus_secs) : std::nullopt;
    // mgr().state_json() already IS the exact JSON payload the React UI
    // expects (same shape the old getStateJSON() returned) — no translation
    // needed, which is the whole point of routing output through JSON.
    std::string j = mgr().state_json(focus);
    return rust::String(j);
}

rust::String get_target_config_json(uint64_t id) {
    return rust::String(mgr().target_config_json(id));
}

rust::String export_target_full_csv(uint64_t id) {
    return rust::String(mgr().export_target_full_csv(id));
}

rust::String export_all_targets_full_csv() {
    return rust::String(mgr().export_all_targets_full_csv());
}

rust::String list_interfaces_json() {
    auto ifs = list_interfaces();
    std::string j = "[";
    for (size_t i = 0; i < ifs.size(); ++i) {
        if (i) j += ",";
        j += "{\"name\":\"" + esc(ifs[i].name) + "\",\"address\":\"" + esc(ifs[i].address)
           + "\",\"v6\":" + (ifs[i].v6 ? "true" : "false") + "}";
    }
    j += "]";
    return rust::String(j);
}

rust::String engine_build() {
    return rust::String(NETPULSE_OBF_STR(NETPULSE_ENGINE_BUILD));
}

void set_data_dir(rust::Str dir) {
    ColdStore::instance().configure(std::string(dir));
}

rust::String ping_start(rust::Str host, double count, bool continuous,
                         double size, double timeout_secs, double ttl,
                         double interval_secs, rust::Str family, bool raw, rust::Str src) {
    std::string host_s(host);
    if (host_s.empty() || host_s.size() > 255) {
        throw std::runtime_error("Invalid host");
    }
    PingConfig cfg;
    cfg.target = host_s;
    cfg.count = static_cast<int>(clampd(count, 1.0, 10000.0));
    cfg.continuous = continuous;
    cfg.payload_size = static_cast<size_t>(clampd(size, 0.0, 65500.0));
    cfg.timeout_secs = clampd(timeout_secs, 0.0, 60.0);
    cfg.ttl = static_cast<uint8_t>(clampd(ttl, 1.0, 255.0));
    cfg.interval_secs = clampd(interval_secs, 0.05, 3600.0);
    std::string fam_s(family);
    cfg.family = fam_s == "v4" ? PingFamilyPref::V4 : fam_s == "v6" ? PingFamilyPref::V6 : PingFamilyPref::Auto;
    cfg.privileged = raw;
    std::string src_s(src);
    if (NETPULSE_OPAQUE(src_s.size() <= 64)) cfg.source_addr = src_s; // ignore absurd input, same as add_target/update_target

    // Purely for display (shown in the UI in place of the old, fake OS
    // shell-command line) — not parsed back by anything, so this can be
    // freely reworded without touching the wire contract.
    std::ostringstream cmd;
    cmd << "native ping -> " << cfg.target
        << " (" << (fam_s.empty() ? "auto" : fam_s) << ", "
        << (cfg.continuous ? std::string("continuous") : (std::to_string(cfg.count) + " probes"))
        << ", " << cfg.payload_size << "B, ttl=" << int(cfg.ttl) << ")";

    uint64_t id = mgr().start_ping(cfg, cmd.str());
    std::string j = "{\"id\":" + std::to_string(id) + ",\"cmd\":\"" + esc(cmd.str()) + "\"}";
    return rust::String(j);
}

void ping_stop(uint64_t id) {
    mgr().stop_ping(id);
}

rust::String ping_poll(uint64_t id) {
    return rust::String(mgr().poll_ping(id));
}

} // namespace netpulse_ffi
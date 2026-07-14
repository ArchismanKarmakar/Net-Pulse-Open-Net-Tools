// netpulse_ffi.cpp — implementation. See netpulse_ffi.hpp for why this file
// exists. Every validation/clamp rule here is copied from napi.cpp's
// settings_from_obj so the engine behaves identically regardless of which
// host process (Electron/Node or Tauri/Rust) is calling it.
#include "netpulse_ffi.hpp"

#include <algorithm>
#include <cmath>
#include <set>
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
    base.max_hops = static_cast<uint8_t>(clampd(maxhops, 1.0, 64.0));

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

rust::String get_state_json(double focus_secs, bool has_focus) {
    std::optional<double> focus = has_focus ? std::optional<double>(focus_secs) : std::nullopt;
    // mgr().state_json() already IS the exact JSON payload the React UI
    // expects (same shape the old getStateJSON() returned) — no translation
    // needed, which is the whole point of routing output through JSON.
    std::string j = mgr().state_json(focus);
    return rust::String(j);
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

} // namespace netpulse_ffi

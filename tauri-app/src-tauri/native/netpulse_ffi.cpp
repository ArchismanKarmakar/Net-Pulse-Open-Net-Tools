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
// BUG FIX: capabilities_json() below calls process_is_elevated() and
// capture_driver_present(), both declared in platform.hpp. This translation
// unit built cleanly in every sandbox verification because a DIFFERENT file
// (session.cpp, part of the same static library) happens to include
// platform.hpp explicitly and got linked in first — nothing here ever
// actually required it. That masked the problem completely until this file
// was compiled fresh with a Windows/MSVC toolchain, which reports each
// translation unit's own missing symbols rather than resolving them from
// whatever the linker happens to see later. The real lesson: a symbol used
// in a .cpp must be declared via that .cpp's OWN #include, never assumed
// from another file in the same build purely because it worked once.
#include "netpulse/platform.hpp"

#ifdef __APPLE__
#  include <AudioToolbox/AudioServices.h> // AudioServicesPlaySystemSound — plain C API, no Objective-C++ needed
#endif

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
                                      rust::Slice<const uint8_t> paused_hops,
                                      rust::Str protocol, double dest_port) {
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

    // TCP intentionally accepted here without rejecting it — Session::run()
    // itself is what refuses to actually probe in TCP mode (surfaces a
    // clear `error_` instead — see its doc comment in session.cpp for
    // exactly why: the reply registry doesn't support TCP's per-TTL varying
    // source ports yet). Rejecting the string here instead would just move
    // the same "not available yet" case to a different, less informative
    // error path.
    std::string proto_s(protocol);
    base.protocol = proto_s == "tcp" ? Protocol::Tcp : proto_s == "udp" ? Protocol::Udp
                    : proto_s == "http" ? Protocol::Http : Protocol::Icmp;
    base.dest_port = static_cast<uint16_t>(clampd(dest_port, 1.0, 65535.0));

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
                     rust::Slice<const uint8_t> paused_hops,
                     rust::Str protocol, double dest_port) {
    std::string t(target);
    if (NETPULSE_OPAQUE(t.empty() || t.size() > 255))
        throw std::invalid_argument("add_target: invalid target"); // cxx turns this into a Rust Result::Err
    Settings st = settings_from_params(Settings{}, probe, trace, timeout, payload, maxhops, raw, family, src, paused_hops, protocol, dest_port);
    return mgr().add(t, st);
}

bool update_target(uint64_t id,
                    double probe, double trace, double timeout, double payload,
                    double maxhops, bool raw,
                    rust::Str family, rust::Str src,
                    rust::Slice<const uint8_t> paused_hops,
                    rust::Str protocol, double dest_port) {
    Settings cur = mgr().settings_of(id).value_or(Settings{});
    Settings st = settings_from_params(cur, probe, trace, timeout, payload, maxhops, raw, family, src, paused_hops, protocol, dest_port);
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
    std::string d(dir);
    ColdStore::instance().configure(d);
    // Point the diagnostic sink at the same directory (see debug_log()'s
    // doc comment, platform.hpp, for why a FILE rather than stderr). Only
    // the PATH is set here — writing stays off until explicitly enabled,
    // so this costs nothing on a normal run.
    if (!d.empty()) {
        const char sep =
#ifdef _WIN32
            '\\';
#else
            '/';
#endif
        debug_log_path() = d + sep + "netpulse-debug.log";
    }
}

// Turn the diagnostic log on/off at runtime and report where it writes.
// Exposed so the UI can offer it as a checkbox — the whole point is that a
// user should not have to set an environment variable in a terminal they
// cannot reach once the app relaunches itself elevated.
rust::String set_debug_logging(bool on) {
    debug_log_enabled().store(on);
    return rust::String(debug_log_path());
}

rust::String ping_start(rust::Str host, double count, bool continuous,
                         double size, double timeout_secs, double ttl,
                         double interval_secs, rust::Str family, bool raw, rust::Str src,
                         rust::Str protocol, double dest_port) {
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
    std::string proto_s(protocol);
    cfg.protocol = proto_s == "udp" ? PingProtocol::Udp
                 : proto_s == "tcp" ? PingProtocol::Tcp
                 : PingProtocol::Icmp; // empty/anything else defaults to icmp, matching the pre-existing (only) behavior
    if (cfg.protocol == PingProtocol::Udp) {
        cfg.udp_dest_port = static_cast<uint16_t>(clampd(dest_port, 1.0, 65535.0));
    } else if (cfg.protocol == PingProtocol::Tcp) {
        cfg.tcp_dest_port = static_cast<uint16_t>(clampd(dest_port, 1.0, 65535.0));
    }

    // Purely for display (shown in the UI in place of the old, fake OS
    // shell-command line) — not parsed back by anything, so this can be
    // freely reworded without touching the wire contract.
    std::ostringstream cmd;
    const char* pname = cfg.protocol == PingProtocol::Udp ? "udp" : cfg.protocol == PingProtocol::Tcp ? "tcp" : "icmp";
    cmd << "native ping (" << pname << ") -> " << cfg.target
        << " (" << (fam_s.empty() ? "auto" : fam_s) << ", "
        << (cfg.continuous ? std::string("continuous") : (std::to_string(cfg.count) + " probes"))
        << ", " << cfg.payload_size << "B";
    if (cfg.protocol == PingProtocol::Udp) cmd << ", port=" << cfg.udp_dest_port;
    else if (cfg.protocol == PingProtocol::Tcp) cmd << ", port=" << cfg.tcp_dest_port;
    else cmd << ", ttl=" << int(cfg.ttl);
    cmd << ")";

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


rust::String capabilities_json() {
    // See the declaration's doc comment (netpulse_ffi.hpp).
    std::string j = std::string("{\"elevated\":") + (process_is_elevated() ? "true" : "false")
                  + ",\"capture\":" + (capture_driver_present() ? "true" : "false")
#ifdef _WIN32
                  + ",\"platform\":\"windows\"}";
#else
                  + ",\"platform\":\"unix\"}";
#endif
    return rust::String(j);
}


rust::String set_debug_logging(bool on, rust::Str dir) {
    // See the declaration's doc comment (netpulse_ffi.hpp).
    std::string d(dir);
    if (!d.empty()) {
        // Caller (Rust) resolves the real per-OS app-data dir and creates it;
        // this just joins the filename with whichever separator that OS uses.
#ifdef _WIN32
        const char sep = '\\';
#else
        const char sep = '/';
#endif
        if (d.back() != sep) d += sep;
        d += "netpulse-debug.log";
        netpulse::debug_log_path() = d;
    }
    netpulse::debug_log_enabled().store(on);
    if (on) {
        netpulse::debug_log("[netpulse] ---- diagnostic logging enabled ----\n");
    } else {
        // Release the file handle now rather than holding it open
        // indefinitely — see debug_log_reset()'s own doc comment.
        netpulse::debug_log_reset();
    }
    return rust::String(netpulse::debug_log_path());
}


void play_alert_sound(rust::Str kind) {
    // See the declaration's doc comment (netpulse_ffi.hpp) for the full
    // rationale — sound only, deliberately no dialog box shown here.
    std::string k(kind);
#ifdef _WIN32
    UINT icon = MB_ICONASTERISK;      // "info" — the mild Windows notification chime
    if (k == "warning") icon = MB_ICONEXCLAMATION; // the same sound a real warning MessageBox would play
    else if (k == "error") icon = MB_ICONHAND;      // the "Critical Stop" sound
    ::MessageBeep(icon);
#elif defined(__APPLE__)
    // kSystemSoundID_UserPreferredAlert respects whatever alert sound the
    // user has actually chosen in System Settings, rather than hardcoding
    // one — macOS doesn't have Windows' distinct 3-tier icon-sound mapping,
    // so this is the closest honest equivalent for all three kinds.
    (void)k;
    AudioServicesPlaySystemSound(kSystemSoundID_UserPreferredAlert);
#else
    // Linux has no single standard alert-sound API across desktop
    // environments (X11's XBell doesn't work under Wayland at all, and
    // pulling in an X11 dependency here purely for a best-effort chime
    // isn't worth the build-complexity cost). Silently a no-op — losing a
    // notification sound is not worth failing the surrounding dialog over.
    (void)k;
#endif
}

} // namespace netpulse_ffi
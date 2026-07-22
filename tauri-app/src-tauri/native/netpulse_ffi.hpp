// netpulse_ffi.hpp — the Rust-facing adapter layer over netpulse::Manager.
//
// WHY THIS FILE EXISTS: cxx (the Rust<->C++ bridge crate) only bridges an
// explicit, restricted set of types — primitives, rust::String/rust::Str,
// rust::Vec/rust::Slice, and opaque pointer types. It does NOT bridge
// std::optional, std::map, std::set, std::function callbacks, or a class with
// embedded std::thread members — all of which netpulse::Session/Manager use
// internally. So, exactly like napi.cpp did for the Node-API boundary, this
// file is a thin, explicit-parameter adapter: every function takes/returns
// only cxx-bridgeable types, and internally calls the SAME netpulse::Manager
// used by the (now-legacy) N-API addon. No engine logic lives here — it's
// pure translation, mirroring napi.cpp's settings_from_obj validation/clamping
// exactly so behavior is identical between the old and new host processes.
//
// Direction in: individual primitive parameters (not a JSON blob) — Tauri's
// #[tauri::command] already deserializes the frontend's call into typed Rust
// values via serde, so passing those straight through to C++ avoids a round
// trip through JSON string parsing that would otherwise require pulling a
// JSON library into the C++ core (which nothing else in this codebase needs).
//
// Direction out: state/interfaces ARE returned as JSON strings, because
// Manager::state_json() already produces exactly that — reusing it costs zero
// new serialization code and keeps the wire format identical to what the
// existing React UI already expects (see ARCHITECTURE.md).
#pragma once

#include "rust/cxx.h"

namespace netpulse_ffi {

// Returns the new session id. Throws (an ordinary C++ exception — cxx
// automatically turns this into a catchable Rust Result::Err) on invalid
// input, exactly like AddTarget in napi.cpp.
uint64_t add_target(rust::Str target,
                     double probe, double trace, double timeout, double payload,
                     double maxhops, bool raw,
                     rust::Str family, rust::Str src,
                     rust::Slice<const uint8_t> paused_hops);

// Partial update: fields not meaningfully different from the sentinel don't
// apply — mirrors settings_from_obj(o, cur) building on the session's CURRENT
// settings. Rust is responsible for passing the session's last-known values
// for anything the frontend didn't change (it already has them from the last
// get_state_json() call), so this function's contract is "set exactly these."
bool update_target(uint64_t id,
                    double probe, double trace, double timeout, double payload,
                    double maxhops, bool raw,
                    rust::Str family, rust::Str src,
                    rust::Slice<const uint8_t> paused_hops);

void pause_target(uint64_t id, bool on);
void stop_target(uint64_t id);
void remove_target(uint64_t id);

// Manually re-trigger route discovery for a target (see
// Session::force_recheck()'s doc comment for the full rationale). No-op if
// the id is unknown.
void force_recheck(uint64_t id);

// has_focus=false means "no focus window" (matches std::nullopt on the C++
// side) — cxx has no Option<T> across the boundary, so a bool + value pair
// stands in for it, same pattern used for every other optional field here.
rust::String get_state_json(double focus_secs, bool has_focus);

// Just one target's "config" object (same shape as get_state_json()'s
// per-target config block) — for a partial-update merge that needs the
// target's current settings WITHOUT paying to build/parse the entire
// multi-target state payload on every slider tweak. "{}" if the id is
// unknown.
rust::String get_target_config_json(uint64_t id);

// Full-fidelity CSV export — see Manager::export_target_full_csv's doc
// comment (manager.hpp) for why this is deliberately NOT built from the
// same downsampled `series` get_state_json() sends the chart. Empty string
// if the id is unknown.
rust::String export_target_full_csv(uint64_t id);
// Same, every target at once (target_id/target_name columns identify which
// rows belong to which target).
rust::String export_all_targets_full_csv();

rust::String list_interfaces_json();

// Build tag of the compiled engine (see NETPULSE_ENGINE_BUILD) — lets the app
// verify at runtime which binary (normal vs. NETPULSE_OBFUSCATE) is loaded,
// same purpose as engineBuild in the old napi/index.js.
rust::String engine_build();

// Sets the on-disk root the hybrid stats store's cold tier writes under (see
// ColdStore::configure in stats.hpp). Call once, early, before any target is
// added — the Rust side calls this in .setup() with the app's own local data
// directory. If never called, the engine self-configures a relative "npdata"
// default the first time it's actually needed.
void set_data_dir(rust::Str dir);

// Native Ping tool — replaces the old OS-`ping`-subprocess implementation
// with the same pooled-socket/shared-dispatcher engine (PingRun,
// ping_run.hpp) the main multi-target monitor uses. Poll-based, matching
// get_state_json's own shape, rather than a push callback across the FFI
// boundary — Rust polls ping_poll() at a tight interval and re-emits each
// new line as a Tauri event (see commands.rs), so the frontend's existing
// event-based contract needs no cxx-callback machinery to support it.
//
// Returns `{"id":<u64>,"cmd":"<human-readable summary>"}` — id to correlate
// with ping_poll() below; cmd purely for display, shown in place of the old
// (fake, OS-specific) shell command line. Throws on an unusable host/config
// (mirrors add_target's convention).
rust::String ping_start(rust::Str host, double count, bool continuous,
                         double size, double timeout_secs, double ttl,
                         double interval_secs, rust::Str family, bool raw, rust::Str src);
void ping_stop(uint64_t id);
// `{"lines":[{"seq":N,"ok":bool,"rtt_ms":number|null,"from":"...","note":"..."}],"done":bool}`
// — every line produced since the LAST poll for this id. See
// Manager::poll_ping's doc comment (manager.hpp) for the full contract,
// including cleanup once a run is done and fully drained.
rust::String ping_poll(uint64_t id);

} // namespace netpulse_ffi
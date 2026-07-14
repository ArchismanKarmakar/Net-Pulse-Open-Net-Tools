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

// has_focus=false means "no focus window" (matches std::nullopt on the C++
// side) — cxx has no Option<T> across the boundary, so a bool + value pair
// stands in for it, same pattern used for every other optional field here.
rust::String get_state_json(double focus_secs, bool has_focus);

rust::String list_interfaces_json();

// Build tag of the compiled engine (see NETPULSE_ENGINE_BUILD) — lets the app
// verify at runtime which binary (normal vs. NETPULSE_OBFUSCATE) is loaded,
// same purpose as engineBuild in the old napi/index.js.
rust::String engine_build();

} // namespace netpulse_ffi

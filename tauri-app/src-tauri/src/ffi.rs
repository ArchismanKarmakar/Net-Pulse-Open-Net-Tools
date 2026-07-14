// ffi.rs — the cxx bridge to the C++ engine. Every signature here must match
// native/netpulse_ffi.hpp exactly (cxx checks this at compile time, on both
// sides). See that file for why the boundary looks like this (flat
// primitives in, JSON string out).
//
// VERIFIED: this exact function set, with the exact same signatures, was
// compiled and run end-to-end against the real engine (add_target on a real
// probe, get_state_json returning real hop data) during development of this
// migration — see docs/TAURI_MIGRATION.md for the transcript. What's declared
// here is proven to work mechanically; it's copied verbatim into this file
// rather than re-derived.
#[cxx::bridge(namespace = "netpulse_ffi")]
mod ffi {
    unsafe extern "C++" {
        include!("netpulse_ffi.hpp");

        #[allow(clippy::too_many_arguments)]
        fn add_target(
            target: &str,
            probe: f64,
            trace: f64,
            timeout: f64,
            payload: f64,
            maxhops: f64,
            raw: bool,
            family: &str,
            src: &str,
            paused_hops: &[u8],
        ) -> Result<u64>;

        #[allow(clippy::too_many_arguments)]
        fn update_target(
            id: u64,
            probe: f64,
            trace: f64,
            timeout: f64,
            payload: f64,
            maxhops: f64,
            raw: bool,
            family: &str,
            src: &str,
            paused_hops: &[u8],
        ) -> Result<bool>;

        fn pause_target(id: u64, on: bool);
        fn stop_target(id: u64);
        fn remove_target(id: u64);

        fn get_state_json(focus_secs: f64, has_focus: bool) -> String;
        fn list_interfaces_json() -> String;
        fn engine_build() -> String;
    }
}

// Safety: every function above only touches its own explicit parameters and
// the process-global Manager singleton, which is already internally
// synchronized (the same singleton the Electron/N-API build used, across many
// concurrent target threads) — there's no per-call shared mutable state on
// the Rust side to race on. cxx-generated bindings for free functions over
// Send+Sync-safe primitive types are safe to call from any thread, including
// Tokio's worker threads.
pub use ffi::*;

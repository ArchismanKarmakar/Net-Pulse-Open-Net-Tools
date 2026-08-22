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
            protocol: &str,
            dest_port: f64,
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
            protocol: &str,
            dest_port: f64,
        ) -> Result<bool>;

        fn pause_target(id: u64, on: bool);
        fn stop_target(id: u64);
        fn remove_target(id: u64);
        fn force_recheck(id: u64);

        fn get_state_json(focus_secs: f64, has_focus: bool) -> String;
        fn get_target_config_json(id: u64) -> String;
        fn export_target_full_csv(id: u64) -> String;
        fn export_all_targets_full_csv() -> String;
        fn list_interfaces_json() -> String;
        fn engine_build() -> String;
        fn set_data_dir(dir: &str);

        // Native Ping tool — see ping_run.hpp (core) and netpulse_ffi.hpp's
        // doc comment on ping_start for the full contract. Replaces the old
        // OS-`ping`-subprocess implementation (tools.rs) with the SAME
        // pooled-socket + shared-dispatcher engine the main multi-target
        // monitor uses, instead of a second, unrelated ICMP mechanism.
        // Runtime capability probe — see netpulse_ffi.hpp's doc comment.
        fn capabilities_json() -> Result<String>;
        // Diagnostic logging toggle — see netpulse_ffi.hpp.
        fn set_debug_logging(on: bool, dir: &str) -> Result<String>;
        // Sound-only OS alert — see netpulse_ffi.hpp.
        fn play_alert_sound(kind: &str);

        #[allow(clippy::too_many_arguments)]
        fn ping_start(
            host: &str,
            count: f64,
            continuous: bool,
            size: f64,
            timeout_secs: f64,
            ttl: f64,
            interval_secs: f64,
            family: &str,
            raw: bool,
            src: &str,
            protocol: &str,
            dest_port: f64,
        ) -> Result<String>;
        fn ping_stop(id: u64);
        fn ping_poll(id: u64) -> String;
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
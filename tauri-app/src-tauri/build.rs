// build.rs — three jobs:
//  1. tauri_build::build() with an AppManifest naming every #[tauri::command]
//     — this is what makes capabilities/default.json's allowlist meaningful:
//     without it, Tauri has no permission entries to allow/deny for the app's
//     OWN commands (as opposed to built-in "core:*" or plugin permissions).
//     Source-verified mechanism: tauri-build::acl::AppManifest, which
//     "autogenerate[s] permissions for each of the app commands" in the form
//     `allow-$command` / `deny-$command` (snake_case command name) — see
//     ../../docs/TAURI_MIGRATION.md for the exact source citation. The ONE
//     detail I could not confirm without a live build is whether capability
//     files reference these unprefixed (`"allow-add_target"`) or with some
//     app-level prefix — Tauri writes the resolved, authoritative list to
//     `gen/schemas/*.json` on first build/`cargo check`; if
//     capabilities/default.json doesn't validate against that schema, that's
//     the fix needed (see the comment there too).
//  2. Compile the C++ engine + adapter via cxx_build (see below).
//  3. Wire NETPULSE_OBFUSCATE (source-level obfuscation, see
//     core/include/netpulse/obfuscate.hpp) through the `obfuscate` Cargo
//     feature — works with any compiler, no special toolchain required.
use std::path::PathBuf;
use tauri_build::{AppManifest, Attributes};

const COMMANDS: &[&str] = &[
    "add_target", "update_target", "pause_target", "stop_target", "remove_target",
    "get_state", "list_interfaces", "engine_build",
    "dns_lookup", "reverse_lookup", "port_scan",
    "ping_start", "ping_stop",
];

fn main() {
    tauri_build::try_build(
        Attributes::new().app_manifest(AppManifest::new().commands(COMMANDS)),
    )
    .expect("failed to run tauri-build (app manifest / command permissions)");

    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    // .../tauri-app/src-tauri -> .../ (repo root) -> core, native
    let repo_root = manifest_dir.parent().unwrap().parent().unwrap().to_path_buf();
    let core_include = repo_root.join("core/include");
    let core_src = repo_root.join("core/src");
    let native_dir = manifest_dir.join("native");

    let mut build = cxx_build::bridge("src/ffi.rs");
    build
        .file(native_dir.join("netpulse_ffi.cpp"))
        .file(core_src.join("icmp.cpp"))
        .file(core_src.join("stats.cpp"))
        .file(core_src.join("transport.cpp"))
        .file(core_src.join("session.cpp"))
        .include(&core_include)
        .include(&native_dir)
        .std("c++20");

    if std::env::var_os("CARGO_FEATURE_OBFUSCATE").is_some() {
        build.define("NETPULSE_OBFUSCATE", None);
    }

    if !cfg!(debug_assertions) {
        // Real, verified hardening for non-obfuscated release builds — same as
        // the CMake side: hide everything but what's actually exported.
        build.flag_if_supported("-fvisibility=hidden");
        build.flag_if_supported("-fvisibility-inlines-hidden");
    }

    if cfg!(windows) {
        build.define("NOMINMAX", None);
    }

    build.compile("netpulse_engine");

    if cfg!(windows) {
        println!("cargo:rustc-link-lib=ws2_32");
        println!("cargo:rustc-link-lib=iphlpapi");
        println!("cargo:rustc-link-lib=winmm");
    } else {
        println!("cargo:rustc-link-lib=pthread");
    }

    println!("cargo:rerun-if-changed={}", native_dir.join("netpulse_ffi.cpp").display());
    println!("cargo:rerun-if-changed={}", native_dir.join("netpulse_ffi.hpp").display());
    println!("cargo:rerun-if-changed=src/ffi.rs");
}

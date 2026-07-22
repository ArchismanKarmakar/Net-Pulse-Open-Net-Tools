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
use tauri_build::{AppManifest, Attributes, WindowsAttributes};

const COMMANDS: &[&str] = &[
    "add_target", "update_target", "pause_target", "stop_target", "remove_target",
    "force_recheck",
    "get_state", "list_interfaces", "engine_build",
    "export_target_csv", "export_all_targets_csv",
    "dns_lookup", "reverse_lookup", "port_scan",
    "ping_start", "ping_stop",
    "write_file", "read_file",
];

// Raw ICMP (SOCK_RAW — the only ICMP mode this app uses on any platform, see
// transport.cpp) requires Administrator on Windows; there is no unprivileged
// alternative (SOCK_DGRAM+ICMP, which Linux supports, simply doesn't exist
// in Winsock). This does NOT mean the whole app should require elevation,
// though — that was tried here previously (requireAdministrator) and caused
// two real problems: (1) it broke `tauri dev` outright, since Windows can't
// silently elevate a child process launched with redirected stdout — which
// is exactly how the dev orchestrator spawns this exe to stream logs, so
// every dev run failed with "the requested operation requires elevation"
// (os error 740) even though the identical exe launched fine standalone;
// (2) it meant a UAC prompt on literally every app launch for every user,
// directly contradicting the NSIS installer hooks (windows/hooks.nsh) built
// specifically to avoid that — those add the Windows Firewall exceptions at
// INSTALL time (already elevated) so raw sockets just work without any
// per-launch prompt. The reactive path (the in-app error banner + firewall-
// detection hint when a socket() call fails) already covers the case where
// elevation genuinely wasn't granted; asInvoker + that existing reactive UX
// is strictly better than forcing elevation on every launch regardless of
// whether the user is even using a feature that needs it.
const WINDOWS_MANIFEST: &str = r#"<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*" />
    </dependentAssembly>
  </dependency>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <asmv3:windowsSettings xmlns="http://schemas.microsoft.com/SMI/2024/WindowsSettings">
      <supportedArchitectures>amd64 arm64</supportedArchitectures>
    </asmv3:windowsSettings>
    <security>
      <requestedPrivileges xmlns="urn:schemas-microsoft-com:asm.v3">
        <requestedExecutionLevel level="asInvoker" uiAccess="false" />
      </requestedPrivileges>
    </security>
  </trustInfo>
</assembly>"#;

fn main() {
    tauri_build::try_build(
        Attributes::new()
            .app_manifest(AppManifest::new().commands(COMMANDS))
            .windows_attributes(WindowsAttributes::new().app_manifest(WINDOWS_MANIFEST)),
    )
    .expect("failed to run tauri-build (app manifest / command permissions)");

    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    // .../tauri-app/src-tauri -> .../ (repo root) -> core, native
    let repo_root = manifest_dir.parent().unwrap().parent().unwrap().to_path_buf();
    let core_include = repo_root.join("core/include");
    let core_src = repo_root.join("core/src");
    let native_dir = manifest_dir.join("native");

    // std::filesystem (stats.cpp — ColdStore's on-disk persistence) is
    // marked explicitly UNAVAILABLE by Apple's libc++ when the deployment
    // target is below macOS 10.15 (Catalina — the first version with actual
    // OS-level support for it); Clang then treats every use as a hard
    // compile error, not a warning. Nothing in this project was explicitly
    // setting a deployment target, so cc-rs (which cxx_build uses
    // internally) fell back to a very old default (10.13) that can't
    // support it. Setting this BEFORE constructing the build below is what
    // actually matters — cc-rs reads MACOSX_DEPLOYMENT_TARGET from the
    // environment to decide which -mmacosx-version-min flag to emit, so
    // this is the single source of truth rather than an extra flag that
    // could conflict with whatever cc-rs would otherwise add on its own.
    if cfg!(target_os = "macos") && std::env::var_os("MACOSX_DEPLOYMENT_TARGET").is_none() {
        // SAFETY: this runs single-threaded, at the very start of the build
        // script, before any other code (including cc-rs's own build steps)
        // could be reading the environment concurrently.
        unsafe { std::env::set_var("MACOSX_DEPLOYMENT_TARGET", "10.15") };
    }

    let mut build = cxx_build::bridge("src/ffi.rs");
    build
        .file(native_dir.join("netpulse_ffi.cpp"))
        .file(core_src.join("icmp.cpp"))
        .file(core_src.join("stats.cpp"))
        .file(core_src.join("transport.cpp"))
        .file(core_src.join("session.cpp"))
        .file(core_src.join("ping_run.cpp"))
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
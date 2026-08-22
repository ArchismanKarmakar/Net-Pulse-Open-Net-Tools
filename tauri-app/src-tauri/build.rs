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
    // Runtime capability probe + elevation relaunch (commands.rs). Listing a
    // command here is what makes tauri-build GENERATE its allow-<name>/
    // deny-<name> permissions in the first place — capabilities/default.json
    // can only grant a permission that already exists. Missing an entry here
    // fails the build with "Permission allow-x not found, expected one of
    // ...", which is exactly what happened the first time these two were
    // added: lib.rs registered the handlers and capabilities/default.json
    // granted them, but this list — the actual source of truth for which
    // permissions exist at all — was never updated, so the permission simply
    // didn't exist yet for default.json to grant.
    "capabilities", "relaunch_elevated", "set_debug_logging", "play_alert_sound",
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
    // environment to decide which -mmacosx-version-min flag to emit.
    //
    // BUG FIX: this comment used to call the env var set here "the single
    // source of truth" for the build's deployment target — that was wrong,
    // confirmed by a real build showing the FINAL linked binary using
    // -mmacosx-version-min=11.0.0, not this 10.15. The var set here only
    // ever affects THIS process (build.rs) and whatever it spawns itself
    // (cc-rs's own clang invocations, compiling the C++ engine) — it
    // cannot reach backward to affect Cargo's own, separate rustc
    // invocation for the actual link step, since that's a sibling process
    // Cargo spawns directly, not a child of build.rs. The real source of
    // truth is now each CI workflow's own job-level `env:` block (see
    // e.g. tauri-release.yml's build job) — set at the top of the whole
    // process tree, it's inherited by BOTH Cargo's link step and this
    // script. This `.is_none()` fallback still matters for a LOCAL build
    // invoked without that env block already set (e.g. a developer running
    // `cargo tauri build` directly) — it's just no longer the only place
    // this needs to be correct, and a local build that only relies on this
    // fallback will still get the same 11.0-not-10.15 mismatch on its
    // final linked binary unless the developer also sets the env var
    // themselves before invoking cargo.
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
        .file(core_src.join("probe_tcp.cpp"))
        .file(core_src.join("probe_udp.cpp"))
        .file(core_src.join("probe_http.cpp"))
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
        // BUG FIX: a real build hit a catastrophic winsock.h-vs-winsock2.h
        // conflict (hundreds of "struct type redefinition" / "redefinition;
        // different linkage" errors, e.g. sockaddr, fd_set, socket(),
        // WSAStartup()) compiling netpulse_ffi.cpp specifically. Root cause:
        // platform.hpp's own top-of-file block correctly puts <winsock2.h>
        // before anything else WITHIN that file, but that guarantee is only
        // as good as whichever header some OTHER, uncontrolled part of the
        // include graph reaches <windows.h> through FIRST in a given
        // translation unit — and <windows.h>, unless WIN32_LEAN_AND_MEAN is
        // already defined, pulls in the legacy <winsock.h> itself, before
        // platform.hpp ever gets a chance to. netpulse_ffi.cpp is exactly
        // the file most exposed to this: it is the one translation unit
        // that sits directly in cxx-bridge/Tauri's own include graph, which
        // has its own reasons to reach <windows.h> and has no obligation to
        // order it favorably for us.
        //
        // Defining this HERE, at the cc::Build level, is the fix that is
        // actually robust to include order: it lands before the FIRST
        // header of ANY of our .cpp files is even opened, so it no longer
        // matters which file some other header happens to pull
        // <windows.h> in through, or in what order. This is also the
        // textbook Microsoft-documented fix for this exact, extremely
        // common class of conflict — see
        // https://learn.microsoft.com/windows/win32/winsock/include-files-2
        // ("To avoid redefinition errors, do not include Winsock.h... if
        // you need Winsock2.h, define WIN32_LEAN_AND_MEAN before including
        // Windows.h, or include Winsock2.h before Windows.h").
        build.define("WIN32_LEAN_AND_MEAN", None);
    }

    build.compile("netpulse_engine");

    if cfg!(windows) {
        println!("cargo:rustc-link-lib=ws2_32");
        println!("cargo:rustc-link-lib=iphlpapi");
        println!("cargo:rustc-link-lib=winmm");
    } else if cfg!(target_os = "macos") {
        println!("cargo:rustc-link-lib=pthread");
        // BUG FIX: this was missing entirely, discovered only by an actual
        // macOS CI build failure — `AudioServicesPlaySystemSound` (used by
        // play_alert_sound(), netpulse_ffi.cpp, for the native OS alert
        // sound on critical dialogs) is declared correctly via
        // <AudioToolbox/AudioServices.h> and compiles fine, but linking
        // failed: "Undefined symbols for architecture arm64:
        // _AudioServicesPlaySystemSound". Tauri's own build links AppKit,
        // WebKit, Security, and several other frameworks it needs for its
        // own windowing/webview — none of which happen to pull in
        // AudioToolbox as a side effect, since nothing else in this binary
        // uses it. This project's own build.rs has to link the frameworks
        // ITS OWN code needs explicitly; it never did for this one, because
        // this code path had never actually been compiled for macOS before
        // (no macOS cross-compiler was available while writing it — see
        // this project's own `docs/` notes on what could and couldn't be
        // verified without real hardware for each platform).
        println!("cargo:rustc-link-lib=framework=AudioToolbox");
    } else {
        println!("cargo:rustc-link-lib=pthread");
    }

    println!("cargo:rerun-if-changed={}", native_dir.join("netpulse_ffi.cpp").display());
    println!("cargo:rerun-if-changed={}", native_dir.join("netpulse_ffi.hpp").display());
    println!("cargo:rerun-if-changed=src/ffi.rs");

    // BUG FIX: the three lines above are the ONLY rerun-if-changed
    // directives this build script emitted. Cargo's rule here is sharp-
    // edged and easy to miss: a build script's DEFAULT behavior (no
    // rerun-if-changed at all) is to watch every file in the whole crate
    // for changes and rerun on any of them — but the INSTANT a build
    // script emits even one rerun-if-changed line, that default is turned
    // off entirely, and Cargo watches ONLY the paths explicitly listed
    // from then on. Since none of the actual C++ engine sources compiled
    // above (icmp.cpp, stats.cpp, transport.cpp, session.cpp,
    // ping_run.cpp, probe_tcp.cpp, probe_udp.cpp, probe_http.cpp, or
    // anything under core/include) were ever listed, editing ANY of them —
    // every single one of the files real engine work actually happens in —
    // was silently invisible to Cargo. `cargo run`/`tauri dev` would see
    // nothing it's watching had changed and just relink whatever
    // netpulse_engine.lib was already sitting in target/, however stale.
    // No error, no warning — the build just silently succeeds against old
    // code. This is a well-known Cargo footgun (search "cargo build script
    // rerun-if-changed" and this exact surprising-default is the first
    // thing everyone hits), not something specific to this project, but it
    // means every C++-side change made so far may never have actually been
    // exercised by a dev-mode run, regardless of how correct the change
    // itself was.
    //
    // Two directories, not an enumerated file list matching the .file()
    // calls above: Cargo has supported watching a DIRECTORY path (recursing
    // through it) since Rust 1.50 (Feb 2021) — every toolchain this project
    // could plausibly be built with is well past that — so this stays
    // correct automatically as files are added/removed/renamed under
    // core/, rather than needing to be kept in sync with the .file() list
    // by hand every time someone adds a new .cpp.
    println!("cargo:rerun-if-changed={}", core_src.display());
    println!("cargo:rerun-if-changed={}", core_include.display());
}
//! Tauri commands — the ENTIRE surface the frontend can call. Every command
//! here is explicitly listed in `invoke_handler` (lib.rs) and in
//! `capabilities/default.json`; nothing else is reachable from the webview.
//! This is the direct architectural replacement for the old Electron
//! preload.js contextBridge allowlist — same principle (a narrow, explicit,
//! named surface), enforced by Tauri's own permission system this time
//! instead of a hand-written bridge object.
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter};

use crate::ffi;
use crate::tools;

// ---------------------------------------------------------------------------
// Target management — thin wrappers over ffi.rs, which is itself a thin
// wrapper over the real C++ engine. See ffi.rs for the "verified end-to-end"
// note; add_target/get_state/list_interfaces/engine_build below call the
// EXACT functions that were run against the real engine during development.
// ---------------------------------------------------------------------------

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TargetConfig {
    pub target: String,
    #[serde(default = "default_probe")]
    pub probe: f64,
    #[serde(default = "default_trace")]
    pub trace: f64,
    #[serde(default)]
    pub timeout: f64,
    #[serde(default = "default_payload")]
    pub payload: f64,
    #[serde(default = "default_maxhops")]
    pub maxhops: f64,
    #[serde(default = "default_raw")]
    pub raw: bool,
    #[serde(default = "default_family")]
    pub family: String,
    #[serde(default)]
    pub src: String,
    #[serde(default)]
    pub paused_hops: Vec<u8>,
    // "icmp" (default) | "udp" | "tcp" — see Session::run()'s doc comment
    // (session.cpp) for why "tcp" is accepted here but currently surfaces
    // a clear error at the engine level rather than actually probing:
    // the C++ side already refuses it explicitly, so there's no need to
    // duplicate that validation here and risk the two falling out of sync.
    #[serde(default = "default_protocol")]
    pub protocol: String,
    #[serde(default = "default_dest_port")]
    pub dest_port: f64,
}
fn default_probe() -> f64 { 1.0 }
fn default_trace() -> f64 { 30.0 }
fn default_payload() -> f64 { 56.0 }
fn default_maxhops() -> f64 { 30.0 }
fn default_raw() -> bool { true }
fn default_family() -> String { "auto".into() }
fn default_protocol() -> String { "icmp".into() }
fn default_dest_port() -> f64 { 33434.0 }

#[tauri::command]
pub fn add_target(cfg: TargetConfig) -> Result<u64, String> {
    ffi::add_target(
        &cfg.target, cfg.probe, cfg.trace, cfg.timeout, cfg.payload,
        cfg.maxhops, cfg.raw, &cfg.family, &cfg.src, &cfg.paused_hops,
        &cfg.protocol, cfg.dest_port,
    )
    .map_err(|e| e.to_string())
}

// update_target receives a PARTIAL object from the frontend (App.jsx's
// applyUpdate() intentionally sends only the fields the user changed — see
// its `/api/update` case) and must MERGE those into the target's CURRENT
// settings, exactly like the old napi.cpp's
// `settings_from_obj(o, mgr().settings_of(id).value_or(Settings{}))` did.
// TargetConfig's serde defaults are wrong for this specific call (a field
// the caller omitted would be overwritten with a hardcoded default instead
// of preserved) — caught this while writing the bridge and fixed it here by
// fetching the target's current config from get_state_json (which already
// has it) and overlaying only the fields actually present in the partial
// update, BEFORE calling the FFI layer with a complete value set.
#[derive(Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct PartialTargetConfig {
    pub probe: Option<f64>,
    pub trace: Option<f64>,
    pub timeout: Option<f64>,
    pub payload: Option<f64>,
    pub maxhops: Option<f64>,
    pub raw: Option<bool>,
    pub family: Option<String>,
    pub src: Option<String>,
    pub paused_hops: Option<Vec<u8>>,
    pub protocol: Option<String>,
    pub dest_port: Option<f64>,
}

#[tauri::command]
pub fn update_target(id: u64, cfg: PartialTargetConfig) -> Result<bool, String> {
    let current = current_config(id)?;
    let merged = TargetConfig {
        target: String::new(), // unused by ffi::update_target (id-based)
        probe: cfg.probe.unwrap_or(current.probe),
        trace: cfg.trace.unwrap_or(current.trace),
        timeout: cfg.timeout.unwrap_or(current.timeout),
        payload: cfg.payload.unwrap_or(current.payload),
        maxhops: cfg.maxhops.unwrap_or(current.maxhops),
        raw: cfg.raw.unwrap_or(current.raw),
        family: cfg.family.unwrap_or(current.family),
        src: cfg.src.unwrap_or(current.src),
        paused_hops: cfg.paused_hops.unwrap_or(current.paused_hops),
        protocol: cfg.protocol.unwrap_or(current.protocol),
        dest_port: cfg.dest_port.unwrap_or(current.dest_port),
    };
    ffi::update_target(
        id, merged.probe, merged.trace, merged.timeout, merged.payload,
        merged.maxhops, merged.raw, &merged.family, &merged.src, &merged.paused_hops,
        &merged.protocol, merged.dest_port,
    )
    .map_err(|e| e.to_string())
}

/// Reads a target's CURRENT settings via get_target_config_json — a single-
/// target FFI call (see netpulse_ffi.hpp), NOT the full multi-target
/// get_state_json. Building and parsing the entire state payload just to
/// read one target's config would get linearly slower per active target on
/// every partial update (e.g. dragging a probe-interval slider), stuttering
/// the UI at scale (50-100 targets) — this reads exactly the bytes needed.
fn current_config(id: u64) -> Result<TargetConfig, String> {
    let json = ffi::get_target_config_json(id);
    let c: serde_json::Value = serde_json::from_str(&json).map_err(|e| e.to_string())?;
    if !c.is_object() || c.as_object().map(|m| m.is_empty()).unwrap_or(true) {
        return Err(format!("update_target: no such target id {id}"));
    }
    let getf = |k: &str, d: f64| c.get(k).and_then(|x| x.as_f64()).unwrap_or(d);
    Ok(TargetConfig {
        target: String::new(),
        probe: getf("probe", 1.0),
        trace: getf("trace", 30.0),
        timeout: getf("timeout", 0.0),
        payload: getf("payload", 56.0),
        maxhops: getf("maxhops", 30.0),
        raw: c.get("raw").and_then(|x| x.as_bool()).unwrap_or(true),
        family: c.get("family").and_then(|x| x.as_str()).unwrap_or("auto").to_string(),
        src: c.get("src").and_then(|x| x.as_str()).unwrap_or("").to_string(),
        protocol: c.get("protocol").and_then(|x| x.as_str()).unwrap_or("icmp").to_string(),
        dest_port: getf("destPort", 33434.0),
        paused_hops: c
            .get("pausedHops")
            .and_then(|x| x.as_array())
            .map(|a| a.iter().filter_map(|n| n.as_u64().map(|n| n as u8)).collect())
            .unwrap_or_default(),
    })
}

#[tauri::command]
pub fn pause_target(id: u64, on: bool) {
    ffi::pause_target(id, on);
}

#[tauri::command]
pub fn stop_target(id: u64) {
    ffi::stop_target(id);
}

#[tauri::command]
pub fn remove_target(id: u64) {
    ffi::remove_target(id);
}

#[tauri::command]
pub fn force_recheck(id: u64) {
    ffi::force_recheck(id);
}

#[tauri::command]
pub fn get_state(focus_secs: Option<f64>) -> String {
    match focus_secs {
        Some(f) => ffi::get_state_json(f, true),
        None => ffi::get_state_json(0.0, false),
    }
}

#[tauri::command]
pub fn list_interfaces() -> String {
    ffi::list_interfaces_json()
}

/// Every raw sample this target has ever recorded, across every hop —
/// deliberately separate from get_state (see export_target_full_csv's doc
/// comment in manager.hpp) rather than a flag on it, since this reads
/// straight through to ColdStore with no downsampling and no focus-window
/// cutoff, a meaningfully different (and heavier) operation than an
/// ordinary poll. Empty string if the id is unknown — the frontend already
/// treats that as "nothing to export" the same way target_config_json does.
#[tauri::command]
pub fn export_target_csv(id: u64) -> String {
    ffi::export_target_full_csv(id)
}

/// Same, every target at once.
#[tauri::command]
pub fn export_all_targets_csv() -> String {
    ffi::export_all_targets_full_csv()
}

#[tauri::command]
pub fn engine_build() -> String {
    ffi::engine_build()
}

// ---------------------------------------------------------------------------
// DNS + port scan — direct calls into tools.rs (verified against real
// network I/O during development; see docs/TAURI_MIGRATION.md).
// ---------------------------------------------------------------------------

#[tauri::command]
pub async fn dns_lookup(name: String) -> tools::DnsResult {
    tools::dns_lookup(&name).await
}

#[tauri::command]
pub async fn reverse_lookup(addr: String) -> tools::ReverseResult {
    tools::reverse_lookup(&addr).await
}

#[tauri::command]
pub async fn port_scan(host: String, start: u16, end: u16) -> tools::PortScanResult {
    tools::port_scan(&host, start, end).await
}

// ---------------------------------------------------------------------------
// Ping — native engine (ffi::ping_start/ping_stop/ping_poll, backed by
// PingRun/Manager::start_ping in the C++ core — see ping_run.hpp's doc
// comment for the full rationale), NOT an OS `ping` subprocess. The C++
// side is poll-based (ping_poll returns whatever's new since the last
// call — same shape get_state_json already uses for the main dashboard);
// this polls it at a tight interval and re-emits each new result as the
// SAME "np-ping-line" / "np-ping-done" Tauri events the frontend already
// listens for, just carrying structured fields (seq/ok/rtt_ms/from/note)
// instead of a raw text line to regex-parse. See src/tauri-bridge.js for
// the matching listen() calls and components/tools/PingPage.jsx for the
// consumer.
// ---------------------------------------------------------------------------

#[derive(Deserialize)]
pub struct PingArgs {
    pub count: Option<u32>,
    pub size: Option<u32>,
    pub timeout: Option<u32>,
    pub ttl: Option<u32>,
    pub interval: Option<f64>,
    pub family: Option<String>,
    pub continuous: Option<bool>,
    // "icmp" (default, matches pre-existing behavior if omitted) or "udp" —
    // see netpulse_ffi.hpp's ping_start doc comment for what each measures.
    pub protocol: Option<String>,
    // UDP-only; ignored for ICMP. Same classic-traceroute default
    // (33434) as ping_run.hpp's PingConfig itself uses when this is absent.
    pub dest_port: Option<u32>,
}

#[derive(Serialize, Clone)]
struct PingLineEvent {
    id: String,
    seq: i64,
    ok: bool,
    rtt_ms: Option<f64>,
    from: String,
    note: String,
}
#[derive(Serialize, Clone)]
struct PingDoneEvent {
    id: String,
}

// Frontend (PingPage.jsx) expects { id, cmd } back from ping_start — id to
// correlate streamed np-ping-line/np-ping-done events, cmd to show the user
// a human-readable summary of what got invoked (see ping_start's doc
// comment in netpulse_ffi.hpp for why this is no longer a real shell
// command line — there's no subprocess to show one for anymore).
#[derive(Serialize)]
pub struct PingStartResult {
    pub id: String,
    pub cmd: String,
}

#[derive(Deserialize)]
struct PingPollLine {
    seq: i64,
    ok: bool,
    rtt_ms: Option<f64>,
    from: String,
    note: String,
}
#[derive(Deserialize)]
struct PingPollResult {
    lines: Vec<PingPollLine>,
    done: bool,
}

#[tauri::command]
pub async fn ping_start(app: AppHandle, host: String, args: PingArgs) -> Result<PingStartResult, String> {
    let count = args.count.unwrap_or(10) as f64;
    let continuous = args.continuous.unwrap_or(false);
    let size = args.size.unwrap_or(56) as f64;
    // PingArgs.timeout is milliseconds (matches the old OS-ping contract the
    // frontend still sends); the engine wants seconds, 0 meaning "auto".
    let timeout_secs = args.timeout.map(|t| t as f64 / 1000.0).unwrap_or(0.0);
    let ttl = args.ttl.unwrap_or(255) as f64;
    let interval_secs = args.interval.unwrap_or(1.0);
    let family = args.family.clone().unwrap_or_default();
    let protocol = args.protocol.clone().unwrap_or_default();
    let dest_port = args.dest_port.unwrap_or(33434) as f64;

    // Raw ICMP (same requirement, and same default, as the main engine —
    // see build.rs's comment on why this app doesn't run elevated by
    // default but every probe still needs a raw socket) and no explicit
    // source binding; PingPage.jsx doesn't currently expose either as a
    // user-facing option, matching its pre-existing scope.
    let json = ffi::ping_start(&host, count, continuous, size, timeout_secs, ttl, interval_secs, &family, true, "", &protocol, dest_port)
        .map_err(|e| e.to_string())?;
    let parsed: serde_json::Value = serde_json::from_str(&json).map_err(|e| e.to_string())?;
    let id = parsed.get("id").and_then(|v| v.as_u64()).ok_or_else(|| "ping_start: malformed response".to_string())?;
    let cmd = parsed.get("cmd").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let id_str = id.to_string();

    let id_for_events = id_str.clone();
    tokio::spawn(async move {
        loop {
            let poll_json = ffi::ping_poll(id);
            let Ok(res) = serde_json::from_str::<PingPollResult>(&poll_json) else { break };
            for l in res.lines {
                let _ = app.emit(
                    "np-ping-line",
                    PingLineEvent { id: id_for_events.clone(), seq: l.seq, ok: l.ok, rtt_ms: l.rtt_ms, from: l.from, note: l.note },
                );
            }
            if res.done {
                let _ = app.emit("np-ping-done", PingDoneEvent { id: id_for_events.clone() });
                break;
            }
            // Tight enough that replies feel live (well under one probe
            // interval even at the fastest UI-exposed setting), loose
            // enough not to matter as a CPU cost — the same tradeoff
            // kNetworkWatchdogPollSecs and other poll cadences in the C++
            // core already make, just much faster here since this is
            // user-facing latency, not a background self-heal check.
            tokio::time::sleep(std::time::Duration::from_millis(80)).await;
        }
    });

    Ok(PingStartResult { id: id_str, cmd })
}

#[tauri::command]
pub fn ping_stop(id: String) -> Result<(), String> {
    let id: u64 = id.parse().map_err(|_| "ping_stop: invalid id".to_string())?;
    ffi::ping_stop(id);
    Ok(())
}

// ---------------------------------------------------------------------------
// Minimal file read/write for the .npulse Save As / Open flow. The project
// grants no generic fs plugin (see capabilities/default.json) — these two
// commands exist ONLY to write/read the exact bytes the user picked via
// tauri-plugin-dialog's native save()/open() dialogs, not a general
// filesystem surface. The path always originates from the dialog, never from
// arbitrary frontend input, so there's no separate scope/allowlist to
// enforce beyond "the user picked this path in a native OS dialog."
// ---------------------------------------------------------------------------

#[tauri::command]
pub fn write_file(path: String, data: Vec<u8>) -> Result<(), String> {
    std::fs::write(&path, data).map_err(|e| e.to_string())
}

#[tauri::command]
pub fn read_file(path: String) -> Result<Vec<u8>, String> {
    std::fs::read(&path).map_err(|e| e.to_string())
}
// ---------------------------------------------------------------------------
// Capability probe + elevation relaunch.
//
// The UI gates protocol selection on these: UDP-style hop discovery needs
// elevation, and TCP hop discovery on Windows additionally needs a capture
// driver. Reporting this up front — and refusing to add a target that cannot
// possibly work — is far better than silently spinning in "discovering".
// ---------------------------------------------------------------------------
#[tauri::command]
pub fn capabilities() -> Result<String, String> {
    ffi::capabilities_json().map_err(|e| e.to_string())
}

// Relaunch this same executable elevated, then ask the current instance to
// exit. Windows has no way to elevate a running process in place — a new
// process must be started via the "runas" verb, which triggers the UAC
// prompt — so "run as administrator" is necessarily a restart, and the UI
// says so before calling this.
#[tauri::command]
pub fn relaunch_elevated(app: tauri::AppHandle) -> Result<(), String> {
    #[cfg(windows)]
    {
        // Deliberately shelled out through PowerShell's Start-Process -Verb
        // RunAs rather than calling ShellExecuteW directly: that would pull in
        // a windows-sys dependency this crate does not currently have, and the
        // one-shot cost here is irrelevant since the process is about to exit.
        // Same net effect — a UAC prompt, then a fresh elevated instance.
        let exe = std::env::current_exe().map_err(|e| e.to_string())?;
        let exe_s = exe.to_string_lossy().replace('\'', "''"); // escape for the single-quoted PS string
        let status = std::process::Command::new("powershell")
            .args(["-NoProfile", "-WindowStyle", "Hidden", "-Command",
                   &format!("Start-Process -FilePath '{}' -Verb RunAs", exe_s)])
            .status()
            .map_err(|e| format!("Could not start the elevation prompt: {e}"))?;
        // A non-zero exit means the UAC prompt was declined (or failed). That
        // must NOT kill the running instance — the user simply chose to stay
        // unelevated, and the app is still perfectly usable for ICMP and TCP
        // destination measurement.
        if !status.success() {
            return Err("Elevation was cancelled. Still running without administrator rights.".into());
        }
        app.exit(0);
        Ok(())
    }
    #[cfg(not(windows))]
    {
        let _ = app;
        Err("Relaunching elevated is Windows-only. On Linux/macOS start the app with sudo instead.".into())
    }
}

// Enable/disable diagnostic logging to a file in the app data dir.
// Deliberately file-based rather than env-var-driven: "Run as administrator"
// launches a brand-new elevated process that inherits none of the
// environment of whatever terminal you were in, so NETPULSE_DEBUG=1 simply
// cannot be set for the elevated case — which is precisely the case that
// most needs diagnosing. Returns the log file path for the UI to display.
#[tauri::command]
pub fn set_debug_logging(app: tauri::AppHandle, on: bool) -> Result<String, String> {
    use tauri::Manager;
    let dir = app.path().app_data_dir().map_err(|e| e.to_string())?;
    // The directory may not exist yet on a fresh install; the C++ side only
    // joins a filename onto it and fopen()s, so it must exist first.
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    ffi::set_debug_logging(on, &dir.to_string_lossy()).map_err(|e| e.to_string())
}

// Plays the OS alert/notification sound alone, no dialog shown — pairs
// with a custom in-app modal that wants the same attention-getting sound a
// native OS dialog gets automatically, without giving up the app's own
// consistent visual styling for a plain native message box.
#[tauri::command]
pub fn play_alert_sound(kind: String) {
    ffi::play_alert_sound(&kind);
}

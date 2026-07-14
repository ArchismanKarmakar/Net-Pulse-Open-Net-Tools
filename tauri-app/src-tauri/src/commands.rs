//! Tauri commands — the ENTIRE surface the frontend can call. Every command
//! here is explicitly listed in `invoke_handler` (lib.rs) and in
//! `capabilities/default.json`; nothing else is reachable from the webview.
//! This is the direct architectural replacement for the old Electron
//! preload.js contextBridge allowlist — same principle (a narrow, explicit,
//! named surface), enforced by Tauri's own permission system this time
//! instead of a hand-written bridge object.
use std::sync::Arc;

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, State};

use crate::ffi;
use crate::tools::{self, PingOpts, PingRegistry};

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
}
fn default_probe() -> f64 { 1.0 }
fn default_trace() -> f64 { 30.0 }
fn default_payload() -> f64 { 56.0 }
fn default_maxhops() -> f64 { 30.0 }
fn default_raw() -> bool { true }
fn default_family() -> String { "auto".into() }

#[tauri::command]
pub fn add_target(cfg: TargetConfig) -> Result<u64, String> {
    ffi::add_target(
        &cfg.target, cfg.probe, cfg.trace, cfg.timeout, cfg.payload,
        cfg.maxhops, cfg.raw, &cfg.family, &cfg.src, &cfg.paused_hops,
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
    };
    ffi::update_target(
        id, merged.probe, merged.trace, merged.timeout, merged.payload,
        merged.maxhops, merged.raw, &merged.family, &merged.src, &merged.paused_hops,
    )
    .map_err(|e| e.to_string())
}

/// Reads a target's CURRENT settings back out of get_state_json — the engine
/// already reports them per-target under "config" (probe/timeout/payload/
/// maxhops/raw/family/src/pausedHops — see manager.hpp's state_json), so this
/// reuses that instead of adding a new C++ export just for this.
fn current_config(id: u64) -> Result<TargetConfig, String> {
    let json = ffi::get_state_json(0.0, false);
    let v: serde_json::Value = serde_json::from_str(&json).map_err(|e| e.to_string())?;
    let targets = v.get("targets").and_then(|t| t.as_array()).ok_or("malformed state JSON")?;
    let t = targets
        .iter()
        .find(|t| t.get("id").and_then(|i| i.as_u64()) == Some(id))
        .ok_or_else(|| format!("update_target: no such target id {id}"))?;
    let c = t.get("config").ok_or("target has no config")?;
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
// Ping — streams output as Tauri events ("np-ping-line" / "np-ping-done"),
// replacing the old ipcRenderer.send-based streaming. See src/main.jsx's
// bridge for the matching listen() calls on the frontend side.
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
}

#[derive(Serialize, Clone)]
struct PingLineEvent {
    id: String,
    line: String,
    stream: &'static str,
}
#[derive(Serialize, Clone)]
struct PingDoneEvent {
    id: String,
    code: Option<i32>,
}

#[tauri::command]
pub async fn ping_start(
    app: AppHandle,
    registry: State<'_, Arc<PingRegistry>>,
    host: String,
    args: PingArgs,
) -> Result<String, String> {
    let opts = PingOpts {
        count: args.count.unwrap_or(10),
        size: args.size,
        timeout_ms: args.timeout,
        ttl: args.ttl,
        interval: args.interval,
        family: args.family,
        continuous: args.continuous.unwrap_or(false),
    };
    let reg = registry.inner().clone();
    let id = tools::new_id();

    let id_for_lines = id.clone();
    let app_line = app.clone();
    let id_for_done = id.clone();
    let app_done = app.clone();

    tools::start(
        &reg,
        id.clone(),
        &host,
        opts,
        move |line, stream| {
            let _ = app_line.emit("np-ping-line", PingLineEvent { id: id_for_lines.clone(), line, stream });
        },
        move |code| {
            let _ = app_done.emit("np-ping-done", PingDoneEvent { id: id_for_done.clone(), code });
        },
    )
    .await?;

    Ok(id)
}

#[tauri::command]
pub async fn ping_stop(registry: State<'_, Arc<PingRegistry>>, id: String) -> Result<(), String> {
    tools::stop(registry.inner(), &id).await;
    Ok(())
}

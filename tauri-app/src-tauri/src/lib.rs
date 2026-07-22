// lib.rs — Tauri app setup: registers every command (see commands.rs). This
// file is the complete list of what the frontend can invoke; cross-check
// against capabilities/default.json, which must name every one of these
// commands explicitly or Tauri's permission system will reject the call at
// runtime even though it's registered here — that's the actual IPC
// protection mechanism (see SECURITY.md's "IPC surface" section).
mod commands;
mod ffi;
mod tools;

use tauri::{Manager as _, WindowEvent};

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_log::Builder::default().build())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_process::init())
        .invoke_handler(tauri::generate_handler![
            commands::add_target,
            commands::update_target,
            commands::pause_target,
            commands::stop_target,
            commands::remove_target,
            commands::force_recheck,
            commands::get_state,
            commands::list_interfaces,
            commands::export_target_csv,
            commands::export_all_targets_csv,
            commands::engine_build,
            commands::dns_lookup,
            commands::reverse_lookup,
            commands::port_scan,
            commands::ping_start,
            commands::ping_stop,
            commands::write_file,
            commands::read_file,
        ])
        .setup(|app| {
            // Desktop-only (mobile has no update mechanism via this plugin —
            // matches Cargo.toml's target-scoped dependency above). Registered
            // here via app.handle() rather than the top-level .plugin() chain
            // because that's what needs the #[cfg(desktop)] gate to compile on
            // any future mobile target.
            #[cfg(desktop)]
            {
                let _ = app.handle().plugin(tauri_plugin_updater::Builder::new().build());
            }

            // Resume-repaint workaround for a known WebView2/Chromium GPU
            // device-loss bug class after OS sleep/lock (no framework-level
            // fix exists yet — see MicrosoftEdge/WebView2Feedback#3111 and
            // tauri-apps/tauri#7142/#8304): a stuck compositor surface reads
            // as a blank window, and this app's dark window background
            // (#0e1116) makes that read as solid black. Focused(true) fires
            // when the window comes back to the foreground after a
            // sleep/lock cycle; nudging the size by 1px and back forces
            // WebView2 to actually repaint instead of showing the stale
            // frame. If this alone doesn't fully resolve it for a given GPU
            // driver, the documented fallback is setting
            // WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--use-angle=warp before
            // launch to route WebView2 off the hardware GPU path entirely.
            // Point the hybrid stats store's cold tier at the app's own local
            // data directory, so long-history samples survive the RAM hot
            // tier without landing in a relative "npdata" folder wherever the
            // process happened to be launched from (see ColdStore::configure).
            if let Ok(dir) = app.path().app_local_data_dir() {
                let _ = std::fs::create_dir_all(&dir);
                ffi::set_data_dir(&dir.to_string_lossy());
            }

            if let Some(window) = app.get_webview_window("main") {
                let repaint_target = window.clone();
                window.on_window_event(move |event| {
                    if let WindowEvent::Focused(true) = event {
                        // Never nudge while maximized: inner_size() here
                        // returns the maximized (near-fullscreen) dimensions,
                        // not a separate "restore" size — Tauri's window API
                        // has no way to read the true pre-maximize size once
                        // already maximized. Applying that fullscreen size
                        // via set_size() (which itself un-maximizes) then
                        // corrupts the window's remembered restore size
                        // permanently: every later un-maximize, even a
                        // manual one, snaps to fill the screen instead of
                        // the configured 1340x880, and since this fires on
                        // every focus-regain, it repeats and visibly
                        // "pops" between states each time. Skipping the
                        // nudge while maximized avoids this class of bug
                        // entirely — the tradeoff is a rarer, purely
                        // cosmetic chance of a stale WebView2 frame while
                        // maximized, which is far preferable to corrupting
                        // window state.
                        if repaint_target.is_maximized().unwrap_or(false) {
                            return;
                        }
                        if let Ok(size) = repaint_target.inner_size() {
                            let mut bumped = size;
                            bumped.width = bumped.width.saturating_add(1);
                            let _ = repaint_target.set_size(tauri::Size::Physical(bumped));
                            let _ = repaint_target.set_size(tauri::Size::Physical(size));
                        }
                    }
                });
            }

            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running Net Pulse (Tauri)");
}
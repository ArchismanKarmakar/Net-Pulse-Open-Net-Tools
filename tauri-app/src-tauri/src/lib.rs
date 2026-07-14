// lib.rs — Tauri app setup: registers every command (see commands.rs) and
// manages the ping-process registry as app state. This file is the complete
// list of what the frontend can invoke; cross-check against
// capabilities/default.json, which must name every one of these commands
// explicitly or Tauri's permission system will reject the call at runtime
// even though it's registered here — that's the actual IPC protection
// mechanism (see SECURITY.md's "IPC surface" section).
mod commands;
mod ffi;
mod tools;

use tools::PingRegistry;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_log::Builder::default().build())
        .manage(PingRegistry::new())
        .invoke_handler(tauri::generate_handler![
            commands::add_target,
            commands::update_target,
            commands::pause_target,
            commands::stop_target,
            commands::remove_target,
            commands::get_state,
            commands::list_interfaces,
            commands::engine_build,
            commands::dns_lookup,
            commands::reverse_lookup,
            commands::port_scan,
            commands::ping_start,
            commands::ping_stop,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Net Pulse (Tauri)");
}

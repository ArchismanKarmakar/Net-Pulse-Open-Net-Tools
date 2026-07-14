// Prevents an additional console window on Windows in release builds — DO NOT REMOVE.
// (Verified pattern: this exact line is in Tauri's own official app template.)
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    netpulse_lib::run();
}

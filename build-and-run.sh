#!/usr/bin/env bash
# NetPulse — one-shot build & run (Linux/macOS).
# NetPulse is a Tauri app; the C++ CMake project only builds the engine
# library + its unit tests. Run REAL probing with elevated privileges (raw sockets):
#   sudo setcap cap_net_raw+ep "$(command -v netpulse)"   # once, Linux, on the installed binary
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
echo "[1/2] Installing tauri-app dependencies..."
( cd "$root/tauri-app" && npm install )
echo "[2/2] Launching NetPulse (cargo build via cxx_build statically links the C++ engine)..."
( cd "$root/tauri-app" && npx tauri dev )

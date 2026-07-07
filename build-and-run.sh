#!/usr/bin/env bash
# NetPulse — one-shot build & run (Linux/macOS).
# NetPulse is an Electron app; the C++ CMake project only builds the engine
# library + its unit tests. Run REAL probing with elevated privileges (raw sockets):
#   sudo setcap cap_net_raw+ep "$(command -v electron)"   # once, Linux
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
echo "[1/4] Building the React UI..."
( cd "$root/web" && npm install && npm run build )
echo "[2/4] Installing Electron..."
( cd "$root/electron" && npm install )
ver="$(node -p "require('$root/electron/node_modules/electron/package.json').version")"
echo "      Electron version detected: $ver"
echo "[3/4] Building the C++ engine as a Node-API addon for Electron $ver..."
rm -f "$root/napi/binding.gyp"; rm -rf "$root/napi/build"  # drop stale node-gyp build (forces C++17)
( cd "$root/napi" && npm install && npx cmake-js rebuild --runtime electron --runtime-version "$ver" )
echo "[4/4] Launching NetPulse..."
( cd "$root/electron" && npm start )

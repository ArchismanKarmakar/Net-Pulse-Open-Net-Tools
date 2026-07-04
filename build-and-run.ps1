# NetPulse — one-shot build & run (Windows PowerShell).
#
# IMPORTANT: NetPulse is an ELECTRON app. Do NOT use VS Code's C++ "Debug/Play"
# button to run it — that only builds/runs the engine's unit tests
# (netpulse_tests.exe), which correctly print "ALL TESTS PASSED" and exit 0.
# That is not the app and is not a crash.
#
# Prereqs: Node.js 18+, CMake 3.15+ on PATH, and a C++17 toolchain (VS Build Tools).
# For REAL ICMP probing, run this in an **Administrator** PowerShell (raw sockets).

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

Write-Host "[1/4] Building the React UI..." -ForegroundColor Cyan
Push-Location "$root\web"; npm install; npm run build; Pop-Location

Write-Host "[2/4] Installing Electron..." -ForegroundColor Cyan
Push-Location "$root\electron"; npm install; Pop-Location

$electronVer = node -p "require('$($root -replace '\\','/')/electron/node_modules/electron/package.json').version"
Write-Host "      Electron version detected: $electronVer" -ForegroundColor DarkGray

Write-Host "[3/4] Building the C++ engine as a Node-API addon for Electron $electronVer..." -ForegroundColor Cyan
Push-Location "$root\napi"; npm install; npx cmake-js compile --runtime electron --runtime-version $electronVer; Pop-Location

Write-Host "[4/4] Launching NetPulse..." -ForegroundColor Green
Push-Location "$root\electron"; npm start; Pop-Location

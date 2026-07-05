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
# Use `rebuild` (clean) rather than `compile` (incremental): a header-only change
# (e.g. core/include/netpulse/manager.hpp) can be missed by an incremental build,
# which silently leaves the OLD engine in place. A clean rebuild guarantees the
# addon reflects the current source every time.
Push-Location "$root\napi"; npm install; npx cmake-js rebuild --runtime electron --runtime-version $electronVer; Pop-Location
$addon = Get-ChildItem -Path "$root\napi\build" -Recurse -Filter *.node -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $addon) { Write-Error "Native addon (.node) was NOT produced — the engine did not build. Fix the C++ build errors above before running."; exit 1 }
Write-Host "      Built native addon: $($addon.FullName)" -ForegroundColor DarkGray

Write-Host "[4/4] Launching NetPulse..." -ForegroundColor Green
Push-Location "$root\electron"; npm start; Pop-Location

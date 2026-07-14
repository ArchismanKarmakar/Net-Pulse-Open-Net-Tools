# NetPulse — one-shot build & run (Windows PowerShell).
#
# IMPORTANT: NetPulse is a TAURI app. Do NOT use VS Code's C++ "Debug/Play"
# button to run it — that only builds/runs the engine's unit tests
# (netpulse_tests.exe), which correctly print "ALL TESTS PASSED" and exit 0.
# That is not the app and is not a crash.
#
# Prereqs: Rust (via rustup), Node.js 20.19+, CMake 3.15+ on PATH, and a
# C++20 toolchain (VS Build Tools). For REAL ICMP probing, run this in an
# **Administrator** PowerShell (raw sockets).
#
# Windows Smart App Control (SAC) note: on a fresh machine, SAC can block a
# native binary in node_modules (e.g. @tauri-apps/cli's *.node binding) or a
# freshly-built cargo artifact the first time it's loaded. That shows up as
# npm's "Cannot find native binding" error, but the real cause is buried in
# the error's [cause]: "An Application Control policy has blocked this
# file." Re-running `npm install` does NOT fix this — the file installed
# fine, SAC just blocks it at load time — so this script detects that case,
# asks before touching anything, and restores SAC afterward either way.

param(
    [switch]$AutoDisableSac  # internal: set when this script re-launches itself elevated after the user already said yes
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$sacPolicyPath = "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy"

function Get-SacState {
    try { (Get-MpComputerStatus -ErrorAction Stop).SmartAppControlState } catch { $null }
}

function Test-IsAdmin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Only ever touches this one value (0=Off, 1=Enforce, 2=Evaluation) and hands
# back whatever it read, so Restore-Sac is an exact, symmetric undo.
function Disable-Sac {
    $original = (Get-ItemProperty $sacPolicyPath -Name VerifiedAndReputablePolicyState -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
    Set-ItemProperty $sacPolicyPath -Name VerifiedAndReputablePolicyState -Value 0 -Type DWord
    & CiTool.exe -r | Out-Null
    Start-Sleep -Seconds 2
    if ((Get-SacState) -ne "Off") { throw "Failed to disable Smart App Control (CiTool refresh didn't apply)." }
    return $original
}

function Restore-Sac($original) {
    if ($null -eq $original) { return }
    Set-ItemProperty $sacPolicyPath -Name VerifiedAndReputablePolicyState -Value $original -Type DWord
    & CiTool.exe -r | Out-Null
    Start-Sleep -Seconds 2
}

$sacOriginal = $null
$sacState = Get-SacState

if ($sacState -and $sacState -ne "Off") {
    $confirmed = $AutoDisableSac
    if (-not $confirmed) {
        Write-Host ""
        Write-Host "Windows Smart App Control is currently: $sacState" -ForegroundColor Yellow
        Write-Host "It can block native binaries in node_modules and freshly-built Rust/cargo artifacts on first run, which npm install alone cannot fix." -ForegroundColor Yellow
        $answer = Read-Host "Temporarily disable Smart App Control for this build, then restore it automatically when done? [y/N]"
        $confirmed = $answer -match '^[Yy]'
    }
    if ($confirmed) {
        if (-not (Test-IsAdmin)) {
            Write-Host "Administrator privileges are required to change Smart App Control. Relaunching elevated..." -ForegroundColor Cyan
            Start-Process powershell.exe -Verb RunAs -ArgumentList @("-NoExit", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-AutoDisableSac")
            exit
        }
        $sacOriginal = Disable-Sac
        Write-Host "Smart App Control disabled for this build; it will be restored automatically when this script exits." -ForegroundColor Green
    } else {
        Write-Host "Continuing without disabling Smart App Control — the build may fail if it blocks a native binary." -ForegroundColor Yellow
    }
}

try {
    Write-Host "[1/2] Installing tauri-app dependencies..." -ForegroundColor Cyan
    Push-Location "$root\tauri-app"; npm install; Pop-Location

    Write-Host "[2/2] Launching NetPulse (cargo build via cxx_build statically links the C++ engine)..." -ForegroundColor Green
    Push-Location "$root\tauri-app"; npx tauri dev; Pop-Location
}
finally {
    if ($null -ne $sacOriginal) {
        Write-Host "Restoring Smart App Control to its previous state..." -ForegroundColor Cyan
        Restore-Sac $sacOriginal
        Write-Host "Smart App Control restored: $(Get-SacState)" -ForegroundColor Green
    }
}

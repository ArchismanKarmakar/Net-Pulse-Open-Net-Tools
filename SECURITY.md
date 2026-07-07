# Security

Net Pulse — Open Net Tools is an open-source (AGPL-3.0) network-diagnostics
desktop app. This document describes how it is hardened, how the native engine
is kept safe, and — importantly — **what it takes to make release builds trusted
by operating systems, antivirus vendors, and VirusTotal.**

## Reporting a vulnerability

Please open a private security advisory on the GitHub repository (or email the
maintainer). Do not file public issues for exploitable vulnerabilities.

---

## How the app is hardened

**Electron / renderer**
- `contextIsolation: true`, `nodeIntegration: false`, `sandbox: true` — the
  renderer cannot touch Node or the filesystem directly.
- All privileged capability crosses a minimal `contextBridge` (`preload.js`);
  the renderer only reaches the engine and tools through explicit IPC channels.
- A strict **Content-Security-Policy** (no remote scripts, no `eval`,
  `object-src 'none'`, `frame-ancestors 'none'`); `connect-src` is limited to
  `self` + `https:` (for RIPEstat/RDAP/DoH routing lookups only).
- `webSecurity: true`, `allowRunningInsecureContent: false`, `webviewTag: false`.
- All in-app navigation and window-open requests are denied; external links open
  in the OS browser via `shell.openExternal`.
- Every renderer permission request/check (camera, mic, geolocation, USB, …) is
  denied — the app needs none.

**Native N-API engine (`napi/`)**
- Every value crossing the N-API boundary is **type-checked and clamped** to a
  safe range (`settings_from_obj` in `napi.cpp`): probe/trace/timeout, payload
  (0–65500), max-hops (1–64), source-address length (≤64), target length
  (≤255). Hostile or garbage input cannot drive huge allocations or unbounded
  loops.
- Every exported function validates arity/argument types and runs inside
  `try/catch`, converting C++ exceptions into JS errors — a bad call throws, it
  never crashes the process.
- The probe engine is deliberately **rate-limited and non-bursty** (global
  token bucket, ~30 pps ceiling, gentle discovery) so it does not resemble an
  ICMP flood/scan to host IDS/behavioural AV. See `core/src/session.cpp`.
- Built with **CMake.js only** (no node-gyp), C++20, `NOMINMAX`, no third-party
  runtime dependencies in the addon.

**Bundled tools (Ping / DNS / Port Scanner)**
- Implemented in the Electron main process using Node built-ins; host input is
  validated against a strict allow-list regex.
- `ping` is spawned with an **argument array (never a shell string)** so input
  can't inject commands.
- The **port scanner** is a bounded TCP-connect scan (≤ 2048 ports per run) and
  is clearly labelled "scan only hosts you own or are authorized to test."

---

## Making release builds trusted (AV / SmartScreen / VirusTotal)

> **Be realistic:** source code and hardening alone **cannot** make an installer
> "trusted" or guarantee an all-clean VirusTotal result. OS/AV trust comes from
> **code signing + reputation**, not from the code being safe. An *unsigned*
> Electron app that opens raw sockets and includes a port scanner *will* trip
> SmartScreen and draw a few heuristic VirusTotal detections no matter how clean
> the code is. The steps below are what actually earn trust.

### 1. Code-sign every artifact (the single most important step)

The build is already wired for signing via `electron/electron-builder.yml` —
you only provide the certificate through environment variables; **no secret is
stored in the repo.**

**Windows (Authenticode).** Use an **OV** or, far better, an **EV** certificate
from a recognized CA (DigiCert, Sectigo, GlobalSign, …).
- File-based (OV `.pfx`):
  ```
  set CSC_LINK=C:\path\to\cert.pfx
  set CSC_KEY_PASSWORD=********
  cd electron && npm run dist:win
  ```
- EV certs live on a hardware token/HSM; sign via your CA's tooling or a custom
  electron-builder sign hook. EV is strongly recommended because it grants
  **instant Microsoft SmartScreen reputation** (OV reputation must accrue over
  many downloads).

**macOS (Developer ID + notarization).** Requires an Apple Developer account.
  ```
  export CSC_LINK=DeveloperID.p12  CSC_KEY_PASSWORD=****
  export APPLE_ID=you@example.com APPLE_APP_SPECIFIC_PASSWORD=**** APPLE_TEAM_ID=XXXX
  # set mac.notarize: true in electron-builder.yml, then:
  cd electron && npm run dist:mac
  ```
  Hardened runtime + notarization + stapling are configured
  (`build/entitlements.mac.plist`); without notarization, Gatekeeper blocks the app.

**Linux.** No central signing authority; ship checksums + a GPG-signed
`SHA256SUMS`, and (optionally) a signed AppImage / repository.

### 2. Build reputation

- Publish releases from a **stable publisher identity** (same cert, same
  `publisherName`, same `appId`) so reputation accumulates.
- Prefer **EV** on Windows for immediate SmartScreen trust.
- Distribute from a consistent, HTTPS origin (GitHub Releases) with checksums.

### 3. Keep heuristics happy

- **Never pack/obfuscate** the binaries (no UPX). Packers are a top false-positive
  trigger. This config uses `compression: normal` and does not pack.
- Ship the same, reproducible artifacts you scan.
- Keep the port scanner and raw-socket features clearly documented and
  user-initiated (they are legitimate diagnostics, but scanners inherently draw
  heuristic attention).

### 4. Handle any residual VirusTotal detections

Even signed apps occasionally get 1–3 heuristic hits from smaller engines. To
clear them:
- Sign first; re-scan the **signed** artifact (unsigned scans are meaningless).
- Submit false-positive reports to the flagging vendors (most have a form):
  Microsoft (Defender), and the specific engines shown on VirusTotal.
- As an open-source project, link the public source and reproducible build in
  the report — vendors whitelist faster when the source is auditable.
- Give reputation a few days/weeks to propagate after the first signed release.

### Checklist for a trusted release

- [ ] `web` built, `napi` built for the target Electron ABI (`npm run build:electron`).
- [ ] `CSC_LINK` / `CSC_KEY_PASSWORD` (and Apple notarization vars on macOS) set.
- [ ] `npm run dist:win` / `dist:mac` / `dist:linux` produced **signed** artifacts.
- [ ] Verified the signature (`signtool verify /pa` on Windows; `spctl -a -vv` on macOS).
- [ ] Published with `SHA256SUMS`; scanned the **signed** file on VirusTotal.
- [ ] Filed false-positive reports for any residual detections.

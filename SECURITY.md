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

**Tauri webview**
- No filesystem, shell, or HTTP plugin is enabled — the only commands the
  frontend can call are the app's own named commands, listed explicitly in
  both `invoke_handler` (`lib.rs`) and `capabilities/default.json`'s
  `permissions` array; anything not listed there is unreachable.
- A strict **Content-Security-Policy** (`tauri.conf.json`): no remote scripts,
  no `eval`, `object-src 'none'`, `frame-ancestors 'none'`.
- All in-app navigation to non-`self` origins is denied; external links open
  in the OS browser.
- devtools are only reachable via the Cargo `devtools` feature, which is
  **not** enabled in release builds.

**Native engine, bridged via `cxx` (`tauri-app/src-tauri/native/netpulse_ffi.cpp`)**
- Every value crossing the Rust↔C++ boundary is **type-checked and clamped**
  to a safe range: probe/trace/timeout, payload (0–65500), max-hops (1–64),
  source-address length (≤64), target length (≤255). Hostile or garbage input
  cannot drive huge allocations or unbounded loops.
- Every FFI function validates its arguments and runs inside `try/catch` on
  the C++ side, converting exceptions into Rust `Result`s — a bad call errors,
  it never crashes the process.
- The probe engine is deliberately **rate-limited and non-bursty** (global
  token bucket, ~30 pps ceiling, gentle discovery) so it does not resemble an
  ICMP flood/scan to host IDS/behavioural AV. See `core/src/session.cpp`.
- Statically linked into the Rust binary via `cxx_build` — no separate
  `.node`/`.so`/`.dylib` addon ships with the app.

**Bundled tools (Ping / DNS / Port Scanner)**
- Implemented in the Rust host using `tokio`/`hickory-resolver` and OS `ping`;
  host input is validated against a strict allow-list before use.
- `ping` is spawned with an **argument array (never a shell string)** so input
  can't inject commands.
- The **port scanner** is a bounded TCP-connect scan (≤ 2048 ports per run) and
  is clearly labelled "scan only hosts you own or are authorized to test."

---

## Reverse engineering / "IP protection" — what's actually achievable

> **Architecture note:** the app is Tauri + Rust (see
> `docs/TAURI_MIGRATION.md`); its IPC surface is protected by Tauri's own
> capability/permission system (`tauri-app/src-tauri/capabilities/default.json`)
> — every command the frontend can call is named explicitly there, nothing
> else is reachable, no filesystem/shell/http plugins are enabled.

This section exists because it's tempting to reach for obfuscation, static
linking, or a full runtime rewrite believing it will "protect the code." It's
worth being precise about what these techniques can and can't do before
investing in them.

**The hard limit.** No client-side technique — obfuscation, packing, statically
linking into a different language, a different app shell — can keep logic
secret from someone who fully controls the device the code runs on. The CPU
has to execute real, decodable instructions to do the work; if it can decode
them, a disassembler (IDA, Ghidra, x64dbg — all free) eventually can too. This
is true of a Node native addon, a Rust binary, a Tauri core, a statically-linked
executable, anything. It is not specific to this app or to Node.js — it's true
of essentially all shipped native code, obfuscated or not. Obfuscation raises
the *cost* of reversing (more analyst-hours, defeats naive automated tools); it
does not remove the possibility.

**This project's actual exposure is small.** There's no embedded secret, API
key, or license-check logic anywhere in the codebase — a traceroute/MTR engine
implements a long-published, RFC-documented technique (vary TTL, read ICMP
Time-Exceeded). Someone motivated to reproduce it doesn't need to reverse the
binary; they can read an RFC, or — since this project is **AGPL-3.0-or-later**
— they're legally entitled to request the exact corresponding source for any
binary they receive (GPL/AGPL §6), and the source is already public on GitHub.
Obfuscating a binary whose matching source sits in the open next to it, under a
license that guarantees that access, doesn't hide anything from a reader
motivated enough to look — they'd just read the repository instead. *(Not
legal advice — a note on how the license mechanically interacts with binary
protection, worth weighing before investing engineering time in obfuscation.)*

**What's implemented, and why it's proportionate:**
- **Release builds strip all symbols** and hide non-required exports
  (`-fvisibility=hidden` + linker `-s` on Unix, `/OPT:REF /OPT:ICF` + no `.pdb`
  on MSVC). Verified: an unstripped build of this engine carries ~3,100 symbols
  and is ~1.1 MB; stripped, it's ~380 KB with 0 local symbols. This is real,
  zero-risk, zero-maintenance-burden hardening — it just removes information a
  binary doesn't need to ship (mangled names, file paths) rather than trying to
  hide logic.
- **DevTools are disabled in packaged builds** — the Cargo `devtools` feature
  is not enabled, so the release binary has no built-in inspector to open.
  This stops casual poking through the window UI; it is not a confidentiality
  boundary (someone can still open the installed files directly), just
  removes the most obvious door.
- **No secrets are embedded** anywhere in the shipped code — checked; nothing
  to leak.

**Obfuscated build variant (`NETPULSE_OBFUSCATE`):** a real out-of-tree LLVM
pass-plugin approach (`ollvm`/Hikari-style IR passes) was attempted and
abandoned — every prebuilt clang toolchain available in testing crashed with
a heap-corruption ABI mismatch when loading a separately-built plugin, and
building LLVM from source just to get a matching ABI isn't worth it given the
AGPL point above. What shipped instead is source-level only
(`core/include/netpulse/obfuscate.hpp`): compile-time string encryption plus
opaque-predicate branches on a handful of literals/validation checks, applied
via preprocessor macros that work with any C++20 compiler. It raises the cost
of a casual `strings`/static-CFG pass; it is not IR-level obfuscation and
does not claim to be. See `docs/OBFUSCATED_BUILD.md` for the full writeup,
including why the plugin path was dropped.

**On statically linking into Rust via Tauri:** this does not itself improve
reverse-engineering resistance over the old Electron/N-API build. The app
ships a compiled native binary (Rust + statically-linked C++) that is exactly
as disassemblable as a `.node` file was — "not a `.node` file sitting right
there with a label on it" has minor obscurity value, but the compiled logic is
equally present and equally extractable either way. Tauri's genuine advantages
are elsewhere, and are the actual reason for the migration: a much smaller
install (no bundled Chromium — it uses the OS's WebView), lower memory use,
and Rust's memory safety in the native layer. Its IPC model (explicit
`invoke`d commands, allowlisted by `capabilities/default.json`) is a clean,
narrow boundary between frontend and native code — equivalent in spirit to
the old Electron setup's `contextIsolation` + `sandbox` + `contextBridge`
allowlist, just enforced by the framework instead of hand-written code.

---



> **Be realistic:** source code and hardening alone **cannot** make an installer
> "trusted" or guarantee an all-clean VirusTotal result. OS/AV trust comes from
> **code signing + reputation**, not from the code being safe. An *unsigned*
> app that opens raw sockets and includes a port scanner *will* trip
> SmartScreen and draw a few heuristic VirusTotal detections no matter how clean
> the code is. The steps below are what actually earn trust — see
> [`CODE_SIGNING.md`](CODE_SIGNING.md) for the full step-by-step runbook
> (certificate types, EV vs OV tradeoffs, notarization, reputation-building,
> and handling residual detections).

### 1. Code-sign every artifact (the single most important step)

`.github/workflows/tauri-release.yml` is already wired for signing via
`tauri-apps/tauri-action@v0` — you only provide the certificate through
repository secrets (`TAURI_SIGNING_PRIVATE_KEY`, `APPLE_CERTIFICATE`,
`WINDOWS_CERTIFICATE`, etc.); **no secret is stored in the repo.**

**Windows (Authenticode).** Use an **OV** or, far better, an **EV** certificate
from a recognized CA (DigiCert, Sectigo, GlobalSign, …), supplied via the
`WINDOWS_CERTIFICATE` / `WINDOWS_CERTIFICATE_PASSWORD` secrets. EV certs live
on a hardware token/HSM; sign via your CA's tooling. EV is strongly
recommended because it grants **instant Microsoft SmartScreen reputation** (OV
reputation must accrue over many downloads).

**macOS (Developer ID + notarization).** Requires an Apple Developer account,
supplied via the `APPLE_CERTIFICATE`, `APPLE_CERTIFICATE_PASSWORD`,
`APPLE_ID`, `APPLE_PASSWORD`, `APPLE_TEAM_ID` secrets — `tauri-action` handles
signing and notarization automatically once these are set. Without
notarization, Gatekeeper blocks the app.

**Linux.** No central signing authority; ship checksums + a GPG-signed
`SHA256SUMS`, and (optionally) a signed AppImage / repository.

### 2. Build reputation

- Publish releases from a **stable publisher identity** (same cert, same
  `publisherName`, same `appId`) so reputation accumulates.
- Prefer **EV** on Windows for immediate SmartScreen trust.
- Distribute from a consistent, HTTPS origin (GitHub Releases) with checksums.

### 3. Keep heuristics happy

- **Never pack** the native binary (no UPX). Packers are a top false-positive
  trigger — `tauri-action`'s bundler does not pack.
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

- [ ] `tauri-app` built (`npx tauri build`) — Rust release binary + installer per OS.
- [ ] Signing secrets (`WINDOWS_CERTIFICATE`/`APPLE_CERTIFICATE`/etc., and Apple
      notarization vars on macOS) set in the repository before tagging a release.
- [ ] `tauri-release.yml` produced **signed** artifacts for each OS.
- [ ] Verified the signature (`signtool verify /pa` on Windows; `spctl -a -vv` on macOS).
- [ ] Published with `SHA256SUMS`; scanned the **signed** file on VirusTotal.
- [ ] Filed false-positive reports for any residual detections.

# NetPulse — Path Latency Studio

A cross-platform **desktop** path-latency monitor (PingPlotter / mtr style):
continuous per-hop ping + traceroute, IPv4 **and** IPv6, multiple targets, live
config, per-hop ASN/BGP, alerts, exports, light & dark themes.

NetPulse is a **native desktop app** — Electron shell + a C++ engine compiled as
an in-process **Node-API addon**. There is **no web server, no localhost port,
and no WebSocket**. The renderer talks to the engine only over Electron IPC,
exactly like a Qt app talks to its backend.

```
┌─────────────────────────────────────────────┐
│ Electron renderer (React UI, web/dist)        │  file://  — no HTTP
│   window.netpulse.*  ──IPC──┐                 │
├─────────────────────────────┼─────────────────┤
│ Electron main (main.js)     ▼                 │
│   require('../napi')  →  netpulse.node         │  in-process
├───────────────────────────────────────────────┤
│ C++ engine (core/)  — ICMP codec, transport,   │  background threads
│ continuous probing, per-hop stats, JSON state  │  raw sockets
└───────────────────────────────────────────────┘
```

## Layout

| Path        | What                                                                 |
|-------------|----------------------------------------------------------------------|
| `core/`     | The engine (header-only API in `core/include/netpulse/`). Pure C++17. |
| `napi/`     | Node-API addon (CMake.js) wrapping the engine. Builds `netpulse.node`. |
| `electron/` | Desktop shell: `main.js` (loads the addon, IPC), `preload.js`.         |
| `web/`      | React renderer (Vite). Built to `web/dist`, loaded over `file://`.     |
| `tests/`    | C++ unit tests for the engine.                                        |
| `CMakeLists.txt` | Builds the core library + unit tests (for engine development).   |

## Prerequisites

- **Node.js** 18+ and npm
- **CMake** 3.15+ on your **PATH** — CMake.js does *not* bundle it.
  - Easiest: install from <https://cmake.org/download/> and tick **"Add CMake to
    the system PATH"**, then open a **new** terminal and check `cmake --version`.
  - Or use the CMake that ships with Visual Studio by launching
    **"Developer PowerShell for VS"** (it puts VS's CMake on PATH).
- A **C++17 compiler**
  - Windows: Visual Studio Build Tools / VS 2022+ — *"Desktop development with C++"*
  - Linux: `build-essential`; macOS: Xcode command-line tools
- The addon is built with **CMake.js**, so it is **independent of the Node.js
  version** (and rebuildable for Electron's ABI).

> `npm install` in `napi/` only fetches dependencies — it does **not** build the
> addon. You build it explicitly (below), after CMake is on your PATH.

## Build & run

### 1. Native engine (Node-API addon)

```bash
cd napi
npm install                  # fetches node-addon-api + cmake-js (no build yet)
npm run build:electron       # builds netpulse.node for Electron's ABI
# (or `npm run build` to build for plain Node, e.g. for the test in index.js)
```

`build:electron` pins Electron 29.1.0; if you use a different Electron, run:
`npx cmake-js compile --runtime electron --runtime-version <your version>`.

### 2. Renderer

```bash
cd web
npm install
npm run build          # → web/dist (assets are relative, for file:// loading)
```

### 3. Desktop app

```bash
cd electron
npm install            # electron
npm start              # launches the app (loads the addon; no server/port)
```

Dev mode (hot-reload UI from Vite while still using the native engine):

```bash
# terminal 1
cd web && npm run dev          # vite on :5173 (UI only — no /api server)
# terminal 2
cd electron && npm run dev     # NETPULSE_DEV=1 electron .  (loads :5173 + addon)
```

### Raw sockets / privileges

ICMP probing needs raw-socket privileges:

- **Windows:** run the app **as Administrator**.
- **Linux:** either run elevated, or grant the capability once:
  `sudo setcap cap_net_raw+ep $(which electron)` (or the packaged binary).
  Alternatively use **unprivileged datagram ICMP** by unchecking **Raw** when
  adding a target (works without admin on most Linux/macOS for plain ping).

## Troubleshooting (Windows)

**`cmake-js` → "CMake is not installed. Install CMake."**
CMake.js found Visual Studio but not a `cmake` executable. Install CMake and add
it to PATH (or use *Developer PowerShell for VS*), open a new terminal, confirm
`cmake --version`, then `cd napi && npm run build:electron`. Note `npm install`
no longer triggers the build, so a missing CMake won't break `npm i`.

**`npm i` in `electron/` → `EBUSY: resource busy or locked … default_app.asar`**
A file lock during Electron's unpack — almost always because the project is in
**`Downloads`/OneDrive** (sync + antivirus lock files mid-rename) or an Electron
process is still running. Fix:
1. Move the project out of `Downloads` to a non-synced path, e.g. `C:\dev\NetPulse`.
2. Close any running `electron.exe`/`node.exe` (Task Manager).
3. `rmdir /s /q electron\node_modules` and `npm cache clean --force`.
4. (Optional) pause OneDrive sync / add a Defender exclusion for the folder.
5. Retry `npm install`.

## Engine development (no Node needed)

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build      # runs the C++ unit tests
```

## Packaging (electron-builder, sketch)

Bundle `web/dist`, the Electron files, and the **Electron-ABI** addon
(`napi/build/Release/netpulse.node` + `napi/index.js`). Mark `napi` as
`asarUnpack` so the `.node` can be `dlopen`ed:

```jsonc
"build": {
  "files": ["electron/**", "web/dist/**", "napi/index.js"],
  "asarUnpack": ["napi/**"],
  "extraResources": ["napi/build/Release/netpulse.node"]
}
```

## Security

- **No network surface from the app itself.** No HTTP server, no open port, no
  WebSocket — the only sockets opened are the ICMP probe sockets.
- **Hardened renderer:** `contextIsolation: true`, `nodeIntegration: false`,
  `sandbox: true`. The renderer reaches the engine only through a minimal,
  audited IPC surface exposed by `preload.js` as `window.netpulse`.
- **Content-Security-Policy** is set in the main process: `default-src 'self'`,
  `script-src 'self'` (no remote scripts, no `eval`), `connect-src 'self'
  https:` (only for the optional BGP/RDAP/DoH lookups), `object-src 'none'`.
- **In-app navigation is blocked**; external links (looking-glass, PeeringDB,
  RIPEstat) open in the OS browser.
- **Addon input validation:** every value crossing the JS↔C++ boundary is
  range-clamped (probe 0.01–3600 s, timeout 0–3600 s, payload 0–1472 B,
  max-hops 1–64) and the target string is length-checked; bad input throws a JS
  error instead of reaching the engine. No shell is ever invoked, so there is no
  command-injection surface.
- Native-addon guidance followed per
  <https://snyk.io/blog/nodejs-add-on-extensions/> (validate at the boundary,
  no untrusted input to native parsing, keep the C++ surface small).

## License / attribution

Independent project; "PingPlotter" and "mtr" are referenced only for comparison
and are trademarks of their respective owners. DejaVu fonts (if bundled) are
under their permissive license.

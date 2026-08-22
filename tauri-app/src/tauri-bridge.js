// tauri-bridge.js — installs window.netpulse with EXACTLY the same shape the
// old electron/preload.js exposed (see that file, kept in the legacy
// electron/ app for reference), so the existing App.jsx — 1600+ lines, tested
// across many iterations — runs UNCHANGED on top of Tauri. Only this file
// and main.jsx differ from the Electron-era frontend; App.jsx and styles.css
// are copied over verbatim.
//
// Two return-shape distinctions vs. the old preload.js, both intentional:
//  - getState / listInterfaces: the Rust commands return the engine's JSON
//    STRING as-is (see native/netpulse_ffi.cpp — it just forwards
//    Manager::state_json()'s existing output), so this bridge JSON.parses
//    them here. The old preload.js's listInterfaces did NOT need a parse
//    because the N-API ListInterfaces built a native JS array directly —
//    that shortcut isn't available through cxx, so parsing moved here.
//  - dns/reverse/portscan: the Rust commands return native structs (via
//    serde), which Tauri serializes to JSON automatically — invoke() already
//    hands back a plain object, no parsing needed.
import { invoke } from '@tauri-apps/api/core'
import { listen } from '@tauri-apps/api/event'
import { save as saveDialog, open as openDialog, message as messageDialog, ask as askDialog } from '@tauri-apps/plugin-dialog'
import { check as checkUpdate } from '@tauri-apps/plugin-updater'
import { relaunch } from '@tauri-apps/plugin-process'
import { getVersion as tauriGetVersion } from '@tauri-apps/api/app'

// invoke()/listen() silently rely on window.__TAURI_INTERNALS__, which only
// exists inside the real Tauri webview process. If this page was opened in a
// plain browser tab (e.g. someone navigated to the Vite dev server URL
// directly instead of going through `npx tauri dev` / the built app), that
// global is undefined and every call below would fail deep inside the SDK
// with a cryptic "Cannot read properties of undefined (reading 'invoke')".
// Fail loudly and specifically here instead.
if (typeof window.__TAURI_INTERNALS__ === 'undefined') {
  const message =
    'NetPulse is not running inside the Tauri app window — it looks like this page was opened directly in a browser. ' +
    'Run it with `npx tauri dev` (or the built NetPulse installer), not by opening this URL in a browser tab.'
  window.netpulse = new Proxy({}, {
    get(_target, prop) {
      if (prop === 'tools') {
        return new Proxy({}, { get: () => () => { throw new Error(message) } })
      }
      return () => { throw new Error(message) }
    },
  })
  document.addEventListener('DOMContentLoaded', () => {
    const banner = document.createElement('div')
    banner.textContent = message
    banner.style.cssText =
      'position:fixed;top:0;left:0;right:0;z-index:99999;background:#b91c1c;color:#fff;' +
      'font:14px/1.4 sans-serif;padding:10px 16px;text-align:center'
    document.body.prepend(banner)
  })
  throw new Error(message)
}

function install() {
  window.netpulse = {
    getState: async (focus) => {
      const json = await invoke('get_state', { focusSecs: typeof focus === 'number' ? focus : null })
      return JSON.parse(json)
    },
    listInterfaces: async () => JSON.parse(await invoke('list_interfaces')),
    // Full-fidelity CSV — every raw sample ColdStore has for a target (or
    // every target), NOT the chart's downsampled series get_state already
    // returns. Plain text back (not JSON — there's nothing to parse, it's
    // already the file content), so the caller can hand it straight to
    // saveFile/writeFile.
    exportTargetCsv: (id) => invoke('export_target_csv', { id }),
    exportAllTargetsCsv: () => invoke('export_all_targets_csv'),
    addTarget: (opts) => invoke('add_target', { cfg: toTargetConfig(opts) }),
    // update_target is a PARTIAL update on the Rust side (see commands.rs's
    // PartialTargetConfig) — it merges only the fields present here into the
    // target's CURRENT settings, so this must NOT fill in defaults the way
    // toTargetConfig() does for a brand-new target; anything App.jsx didn't
    // set (e.g. `undefined`) must stay ABSENT from the object entirely, since
    // JSON.stringify already drops `undefined` values (serde then sees the
    // key as simply not present, i.e. None), rather than being coerced to a
    // default that would incorrectly overwrite the target's real setting.
    updateTarget: (id, opts) => invoke('update_target', { id, cfg: toPartialTargetConfig(opts) }),
    pauseTarget: (id, on) => invoke('pause_target', { id, on: !!on }),
    stopTarget: (id) => invoke('stop_target', { id }),
    removeTarget: (id) => invoke('remove_target', { id }),
    forceRecheck: (id) => invoke('force_recheck', { id }),
    engineBuild: () => invoke('engine_build'),

    // Native Save As / Open dialogs for the .npulse target-list format (see
    // App.jsx's exportTargetList/importTargetList) — saveFile/openFile return
    // null if the user cancels, matching browser showSaveFilePicker()'s own
    // cancel-throws-AbortError convention loosely (null is simpler to check
    // than a try/catch at every call site).
    saveFile: async (defaultName, filters) => {
      const path = await saveDialog({ defaultPath: defaultName, filters })
      return path || null
    },
    openFile: async (filters) => {
      const path = await openDialog({ multiple: false, filters })
      return path || null
    },
    writeFile: (path, bytes) => invoke('write_file', { path, data: Array.from(bytes) }),
    readFile: async (path) => new Uint8Array(await invoke('read_file', { path })),

    tools: {
      dns: (name) => invoke('dns_lookup', { name }),
      reverse: (addr) => invoke('reverse_lookup', { addr }),
      portscan: (host, s, e) => invoke('port_scan', { host, start: s, end: e }),
      pingStart: (host, opts) => invoke('ping_start', { host, args: toPingArgs(opts) }),
      pingStop: (id) => invoke('ping_stop', { id }),
      // Runtime capability probe + elevation relaunch — see commands.rs.
      capabilities: () => invoke('capabilities').then((s) => JSON.parse(s)),
      relaunchElevated: () => invoke('relaunch_elevated'),
      // Returns the log file path. See commands.rs for why this is a file
      // toggle rather than an environment variable.
      setDebugLogging: (on) => invoke('set_debug_logging', { on: !!on }),
      // Sound only, no dialog — see commands.rs.
      playAlertSound: (kind) => invoke('play_alert_sound', { kind: kind || 'info' }),
      // NATIVE OS dialogs (not the in-app React modal). Used for blocking
      // prerequisite prompts because a native dialog gives three things the
      // custom modal structurally cannot: it is genuinely application-modal
      // (cannot be dismissed by clicking the page behind it), it plays the
      // platform's own alert sound, and it renders with the OS's native
      // warning/error iconography — identically on Windows, macOS and Linux.
      // `kind` is 'info' | 'warning' | 'error'.
      nativeMessage: (message, { title, kind = 'warning' } = {}) => messageDialog(message, { title, kind }),
      // Returns true/false. okLabel/cancelLabel let the two choices read as
      // real actions ('Restart as Administrator' / 'Cancel') rather than a
      // bare Yes/No.
      nativeAsk: (message, { title, kind = 'warning', okLabel, cancelLabel } = {}) =>
        askDialog(message, { title, kind, okLabel, cancelLabel }),
      onPingLine: (cb) => {
        let unlisten = null
        let cancelled = false
        listen('np-ping-line', (event) => cb(event.payload)).then((fn) => {
          if (cancelled) fn()
          else unlisten = fn
        }).catch((e) => {
          console.error('Net Pulse: failed to listen for ping output —', e)
          cb({ seq: 0, ok: false, rtt_ms: null, from: '', note: `Failed to receive ping output: ${e}` })
        })
        return () => { cancelled = true; if (unlisten) unlisten() }
      },
      onPingDone: (cb) => {
        let unlisten = null
        let cancelled = false
        listen('np-ping-done', (event) => cb(event.payload)).then((fn) => {
          if (cancelled) fn()
          else unlisten = fn
        }).catch((e) => {
          console.error('Net Pulse: failed to listen for ping completion —', e)
          cb({ id: null })
        })
        return () => { cancelled = true; if (unlisten) unlisten() }
      },
    },

    // Reads the version tauri.conf.json was actually built with — used by
    // the About box so it can never drift out of sync with a real release
    // the way a hand-typed version string in App.jsx eventually would.
    getVersion: async () => {
      try { return await tauriGetVersion() } catch { return null }
    },

    // Auto-update: check() returns { available, version, date, body } or
    // { available: false }; on failure (offline, GitHub unreachable, no
    // matching release yet) it also returns { available: false } rather than
    // throwing — a missed update check should never surface as an error to
    // the user, just silently try again next time. install(onProgress)
    // downloads, verifies the signature (the plugin itself refuses anything
    // not signed with the configured pubkey — this cannot be bypassed from
    // here), installs, and relaunches; the caller doesn't need to know any
    // of that, just call it after check() said one's available.
    updater: {
      check: async () => {
        try {
          const u = await checkUpdate()
          if (!u) return { available: false }
          return { available: true, version: u.version, date: u.date || null, body: u.body || '', _update: u }
        } catch (e) {
          return { available: false, error: String(e) }
        }
      },
      install: async (info, onProgress) => {
        if (!info || !info._update) return { ok: false, error: 'No update to install' }
        try {
          let downloaded = 0
          let total = 0
          await info._update.downloadAndInstall((event) => {
            if (event.event === 'Started') { total = event.data.contentLength || 0 }
            else if (event.event === 'Progress') { downloaded += event.data.chunkLength || 0 }
            if (onProgress) onProgress({ downloaded, total })
          })
          await relaunch()
          return { ok: true }
        } catch (e) {
          return { ok: false, error: String(e) }
        }
      },
    },
  }
}

// addTarget/updateTarget's `opts` (from App.jsx) has fields named to match the
// OLD napi.cpp options object (target/probe/trace/timeout/payload/maxhops/
// raw/family/src/pausedHops) — same names the new Rust TargetConfig expects
// (with #[serde(rename_all = "camelCase")] on the Rust side for pausedHops),
// so this is mostly pass-through; it exists to fill in defaults so a partial
// updateTarget(id, { probe: 2 }) call doesn't send `undefined` for the rest.
function toTargetConfig(opts) {
  return {
    target: opts.target ?? '',
    probe: numOr(opts.probe, 1),
    trace: numOr(opts.trace, 30),
    timeout: numOr(opts.timeout, 0),
    payload: numOr(opts.payload, 56),
    maxhops: numOr(opts.maxhops, 30),
    raw: opts.raw !== undefined ? !!opts.raw : true,
    family: opts.family || 'auto',
    src: opts.src || '',
    pausedHops: Array.isArray(opts.pausedHops) ? opts.pausedHops : [],
    protocol: opts.protocol || 'icmp',
    destPort: numOr(opts.destPort, 33434),
  }
}
function toPingArgs(opts) {
  return {
    count: opts?.count, size: opts?.size, timeout: opts?.timeout, ttl: opts?.ttl,
    interval: opts?.interval, family: opts?.family, continuous: opts?.continuous,
    protocol: opts?.protocol, dest_port: opts?.destPort,
  }
}
function toPartialTargetConfig(opts) {
  const o = {}
  if (opts.probe != null) o.probe = numOr(opts.probe, undefined)
  if (opts.trace != null) o.trace = numOr(opts.trace, undefined)
  if (opts.timeout != null) o.timeout = numOr(opts.timeout, undefined)
  if (opts.payload != null) o.payload = numOr(opts.payload, undefined)
  if (opts.maxhops != null) o.maxhops = numOr(opts.maxhops, undefined)
  if (opts.raw !== undefined) o.raw = !!opts.raw
  if (opts.family) o.family = opts.family
  if (opts.src !== undefined) o.src = opts.src
  if (opts.protocol) o.protocol = opts.protocol
  if (opts.destPort != null) o.destPort = numOr(opts.destPort, undefined)
  if (Array.isArray(opts.pausedHops)) o.pausedHops = opts.pausedHops
  return o
}
function numOr(v, d) { const n = Number(v); return Number.isFinite(n) ? n : d }

install()
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
    engineBuild: () => invoke('engine_build'),

    tools: {
      dns: (name) => invoke('dns_lookup', { name }),
      reverse: (addr) => invoke('reverse_lookup', { addr }),
      portscan: (host, s, e) => invoke('port_scan', { host, start: s, end: e }),
      pingStart: (host, opts) => invoke('ping_start', { host, args: toPingArgs(opts) }),
      pingStop: (id) => invoke('ping_stop', { id }),
      onPingLine: (cb) => {
        let unlisten = null
        let cancelled = false
        listen('np-ping-line', (event) => cb(event.payload)).then((fn) => {
          if (cancelled) fn()
          else unlisten = fn
        })
        return () => { cancelled = true; if (unlisten) unlisten() }
      },
      onPingDone: (cb) => {
        let unlisten = null
        let cancelled = false
        listen('np-ping-done', (event) => cb(event.payload)).then((fn) => {
          if (cancelled) fn()
          else unlisten = fn
        })
        return () => { cancelled = true; if (unlisten) unlisten() }
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
  }
}
function toPingArgs(opts) {
  return {
    count: opts?.count, size: opts?.size, timeout: opts?.timeout, ttl: opts?.ttl,
    interval: opts?.interval, family: opts?.family, continuous: opts?.continuous,
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
  if (Array.isArray(opts.pausedHops)) o.pausedHops = opts.pausedHops
  return o
}
function numOr(v, d) { const n = Number(v); return Number.isFinite(n) ? n : d }

install()

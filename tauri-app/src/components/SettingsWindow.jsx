// SettingsWindow.jsx — the app-wide Settings window (FEATURE, user-requested):
// a SEPARATE OS window (see commands.rs's open_settings_window and
// main.jsx's `?window=settings` branch — not a panel or modal inside the
// main window), whose values are saved to disk and reloaded automatically
// the next time the app starts (see commands.rs's AppSettings/
// load_app_settings/save_app_settings doc comments for the full mechanism
// and where the file lives).
//
// Covers two genuinely different kinds of setting, both asked for
// explicitly:
//  1. The passive background "auto refresh" engine tuning (how often a
//     confirmed hop gets re-verified, and how many misses before a stale
//     route is wiped and rediscovered — see ARCHITECTURE.md §6 and
//     session.hpp's set_default_recheck_tuning doc comment for the full
//     mechanism this changes). Defaults to 30s / 2 misses.
//  2. What a freshly opened "Add target" form on the MAIN window starts
//     pre-filled with — these used to be hardcoded literals in App.jsx's
//     `form` useState; now they're read from here instead.
// Saving here takes effect immediately (the auto-refresh values are pushed
// straight into the running engine, live) and is broadcast to the main
// window via a Tauri event so its own "Add target" form/theme update
// without needing an app restart.
import React, { useEffect, useState } from 'react'

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v))

// Mirrors the C++ side's own clamp range exactly (session.cpp's
// kMinRecheckSecs/kMaxRecheckSecs/kMin|MaxLegacyMissThreshold) — shown here
// too so the form's own min/max hints match what the engine will actually
// do with an out-of-range value, rather than silently disagreeing with it.
const RECHECK_SECS_MIN = 5, RECHECK_SECS_MAX = 300
const RECHECK_THRESHOLD_MIN = 2, RECHECK_THRESHOLD_MAX = 10

const DEFAULTS = {
  autoRefreshSecs: 30,
  autoRefreshThreshold: 2,
  defaultProbe: 1,
  defaultTrace: 30,
  defaultTimeout: 0,
  defaultPayload: 56,
  defaultMaxhops: 30,
  defaultRaw: true,
  defaultFamily: 'auto',
  defaultProtocol: 'icmp',
  defaultDestPort: 33434,
  theme: 'dark',
}

function Field({ label, hint, children }) {
  return (
    <label style={{ display: 'flex', flexDirection: 'column', gap: 4, fontSize: 13 }}>
      <span style={{ opacity: 0.85 }}>{label}</span>
      {children}
      {hint && <span style={{ fontSize: 11, opacity: 0.55 }}>{hint}</span>}
    </label>
  )
}

export default function SettingsWindow() {
  const [values, setValues] = useState(DEFAULTS)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [saved, setSaved] = useState(null) // { ok: bool, message: string } | null
  const [applied, setApplied] = useState(null) // { windowSecs, threshold } from getRecheckTuning(), post-save confirmation
  const [loadError, setLoadError] = useState(null)

  useEffect(() => {
    document.documentElement.dataset.theme = values.theme
  }, [values.theme])

  useEffect(() => {
    let cancelled = false
    window.netpulse.loadAppSettings()
      .then((s) => { if (!cancelled) setValues({ ...DEFAULTS, ...s }) })
      .catch((e) => { if (!cancelled) setLoadError(String(e && e.message || e)) })
      .finally(() => { if (!cancelled) setLoading(false) })
    return () => { cancelled = true }
  }, [])

  const set = (key) => (e) => {
    const el = e.target
    const v = el.type === 'checkbox' ? el.checked : el.type === 'number' ? Number(el.value) : el.value
    setValues((cur) => ({ ...cur, [key]: v }))
  }

  const save = async () => {
    setSaving(true)
    setSaved(null)
    try {
      // Client-side clamp before sending — matches the engine's own clamp
      // exactly (session.cpp) so the form never shows a value the engine
      // will silently override to something else right after Save.
      const toSave = {
        ...values,
        autoRefreshSecs: clamp(Number(values.autoRefreshSecs) || DEFAULTS.autoRefreshSecs, RECHECK_SECS_MIN, RECHECK_SECS_MAX),
        autoRefreshThreshold: Math.round(clamp(Number(values.autoRefreshThreshold) || DEFAULTS.autoRefreshThreshold, RECHECK_THRESHOLD_MIN, RECHECK_THRESHOLD_MAX)),
      }
      await window.netpulse.saveAppSettings(toSave)
      setValues(toSave)
      window.netpulse.emitSettingsChanged(toSave).catch(() => {})
      // Read back what the engine actually applied, as a genuine
      // confirmation rather than just trusting the save call didn't throw —
      // see get_recheck_tuning's own doc comment (commands.rs) for why this
      // check exists at all.
      const conf = await window.netpulse.getRecheckTuning().catch(() => null)
      setApplied(conf)
      setSaved({ ok: true, message: 'Settings saved and applied.' })
    } catch (e) {
      setSaved({ ok: false, message: String(e && e.message || e) })
    } finally {
      setSaving(false)
    }
  }

  const resetDefaults = () => { setValues(DEFAULTS); setSaved(null); setApplied(null) }
  const close = () => { try { window.close() } catch {} }

  if (loading) {
    return <div style={{ padding: 24, fontFamily: 'inherit' }}>Loading settings…</div>
  }

  return (
    <div style={{
      padding: '18px 22px 22px', display: 'flex', flexDirection: 'column', gap: 20,
      color: 'var(--text)', background: 'var(--bg)', minHeight: '100vh', fontSize: 13.5,
    }}>
      <div>
        <h2 style={{ margin: '0 0 4px', fontSize: 17 }}>Settings</h2>
        <div style={{ opacity: 0.65, fontSize: 12 }}>Saved to disk — reloaded automatically every time Net Pulse starts.</div>
      </div>

      {loadError && (
        <div style={{ padding: '8px 12px', borderRadius: 8, background: 'color-mix(in srgb, #a13d1f 20%, transparent)', fontSize: 12.5 }}>
          Couldn't load saved settings ({loadError}) — showing built-in defaults.
        </div>
      )}

      <section style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <h3 style={{ margin: 0, fontSize: 13, textTransform: 'uppercase', letterSpacing: 0.4, opacity: 0.6 }}>
          Auto-refresh (background stale-route recheck)
        </h3>
        <div style={{ fontSize: 12, opacity: 0.65, lineHeight: 1.5 }}>
          How often an already-resolved hop is quietly re-verified, and how many
          misses in a row before it's treated as a real route change and
          rediscovered. This is the passive background mechanism — separate
          from, and much slower by design than, pressing Force Recheck on a
          target.
        </div>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
          <Field label="Frequency (seconds)" hint={`${RECHECK_SECS_MIN}–${RECHECK_SECS_MAX}s, default 30`}>
            <input type="number" min={RECHECK_SECS_MIN} max={RECHECK_SECS_MAX} step="1"
              value={values.autoRefreshSecs} onChange={set('autoRefreshSecs')} />
          </Field>
          <Field label="Misses before re-verify" hint={`${RECHECK_THRESHOLD_MIN}–${RECHECK_THRESHOLD_MAX}, default 2`}>
            <input type="number" min={RECHECK_THRESHOLD_MIN} max={RECHECK_THRESHOLD_MAX} step="1"
              value={values.autoRefreshThreshold} onChange={set('autoRefreshThreshold')} />
          </Field>
        </div>
        {applied && (
          <div style={{ fontSize: 11.5, opacity: 0.6 }}>
            Currently applied in the engine: {applied.windowSecs}s / {applied.threshold} misses.
          </div>
        )}
      </section>

      <section style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <h3 style={{ margin: 0, fontSize: 13, textTransform: 'uppercase', letterSpacing: 0.4, opacity: 0.6 }}>
          New target defaults
        </h3>
        <div style={{ fontSize: 12, opacity: 0.65, lineHeight: 1.5 }}>
          What a freshly opened "Add target" form starts pre-filled with.
          Targets already added keep whatever they were given at the time —
          changing this only affects new ones.
        </div>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
          <Field label="Probe interval (s)">
            <input type="number" min="0.1" step="0.1" value={values.defaultProbe} onChange={set('defaultProbe')} />
          </Field>
          <Field label="Trace interval (s)">
            <input type="number" min="1" step="1" value={values.defaultTrace} onChange={set('defaultTrace')} />
          </Field>
          <Field label="Timeout (s)" hint="0 = auto">
            <input type="number" min="0" step="0.1" value={values.defaultTimeout} onChange={set('defaultTimeout')} />
          </Field>
          <Field label="Payload size (bytes)">
            <input type="number" min="0" step="1" value={values.defaultPayload} onChange={set('defaultPayload')} />
          </Field>
          <Field label="Max hops">
            <input type="number" min="1" max="255" step="1" value={values.defaultMaxhops} onChange={set('defaultMaxhops')} />
          </Field>
          <Field label="Destination port" hint="TCP/UDP only">
            <input type="number" min="1" max="65535" step="1" value={values.defaultDestPort} onChange={set('defaultDestPort')} />
          </Field>
          <Field label="Family">
            <select value={values.defaultFamily} onChange={set('defaultFamily')}>
              <option value="auto">Auto</option>
              <option value="v4">IPv4</option>
              <option value="v6">IPv6</option>
            </select>
          </Field>
          <Field label="Protocol">
            <select value={values.defaultProtocol} onChange={set('defaultProtocol')}>
              <option value="icmp">ICMP</option>
              <option value="udp">UDP</option>
              <option value="tcp">TCP</option>
            </select>
          </Field>
        </div>
        <label style={{ display: 'flex', alignItems: 'center', gap: 8, fontSize: 13 }}>
          <input type="checkbox" checked={!!values.defaultRaw} onChange={set('defaultRaw')} />
          Raw/privileged mode by default
        </label>
      </section>

      <section style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
        <h3 style={{ margin: 0, fontSize: 13, textTransform: 'uppercase', letterSpacing: 0.4, opacity: 0.6 }}>
          Appearance
        </h3>
        <div style={{ display: 'flex', gap: 16 }}>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
            <input type="radio" name="theme" value="dark" checked={values.theme === 'dark'} onChange={set('theme')} /> Dark
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
            <input type="radio" name="theme" value="light" checked={values.theme === 'light'} onChange={set('theme')} /> Light
          </label>
        </div>
      </section>

      {saved && (
        <div style={{
          padding: '8px 12px', borderRadius: 8, fontSize: 12.5,
          background: saved.ok ? 'color-mix(in srgb, #1f7a3d 20%, transparent)' : 'color-mix(in srgb, #a13d1f 20%, transparent)',
        }}>
          {saved.ok ? saved.message : `Couldn't save settings: ${saved.message}`}
        </div>
      )}

      <div style={{ display: 'flex', gap: 10, justifyContent: 'flex-end', marginTop: 4 }}>
        <button onClick={resetDefaults} disabled={saving}>Reset to defaults</button>
        <button onClick={close} disabled={saving}>Close</button>
        <button onClick={save} disabled={saving} className="primary">{saving ? 'Saving…' : 'Save'}</button>
      </div>
    </div>
  )
}

// SettingsPage.jsx — the app-wide Settings tab (FEATURE, user-requested;
// SUPERSEDES the earlier separate-OS-window version).
//
// History: this used to be a genuinely separate OS window, opened via
// commands.rs's open_settings_window and a `?window=settings` query-param
// branch in main.jsx. In real-world testing that window came up blank, and
// — because Tauri doesn't tie a secondary window's lifetime to the main
// one — closing the main window left the whole process running in the
// background with no visible window left to close it from. Rather than
// debug that window's content-loading path further, this is a tab in the
// SAME window instead, right beside "Interfaces": it can't go blank
// independently of the rest of the app (it's the same page, the same JS
// context, the same webview), and there is no second window to outlive the
// first, so both symptoms are gone by construction. See CHANGELOG.md's
// "Settings moved from a separate window into a tab" entry for the full
// writeup.
//
// Still covers the same two genuinely different kinds of setting:
//  1. The passive background "auto refresh" engine tuning (how often a
//     confirmed hop gets re-verified, and how many misses before a stale
//     route is wiped and rediscovered — see ARCHITECTURE.md §6 and
//     session.hpp's set_default_recheck_tuning doc comment for the full
//     mechanism this changes). Defaults to 30s / 2 misses.
//  2. What a freshly opened "Add target" form on this SAME window starts
//     pre-filled with (App.jsx's `form` useState reads these once at
//     mount and again live on every Save here, via the existing
//     onSettingsChanged broadcast — see App.jsx's own effect for that).
// Saving here takes effect immediately (the auto-refresh values are pushed
// straight into the running engine, live) and updates the "Add target"
// form/theme on this same page without needing an app restart.
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

export default function SettingsPage() {
  const [values, setValues] = useState(DEFAULTS)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [saved, setSaved] = useState(null) // { ok: bool, message: string } | null
  const [applied, setApplied] = useState(null) // { windowSecs, threshold } from getRecheckTuning(), post-save confirmation
  const [loadError, setLoadError] = useState(null)

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
      // Same event App.jsx already listens for (onSettingsChanged) to
      // refresh the "Add target" form's defaults and the theme — kept even
      // though Settings now lives in the same window, since App.jsx's own
      // hydration effect already subscribes to it and this avoids having
      // to prop-drill `form`/`theme` setters down into this page instead.
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

  if (loading) {
    return (
      <div className="toolpage fluid">
        <div className="toolpage-header"><h2>Settings</h2></div>
        <div className="muted">Loading settings…</div>
      </div>
    )
  }

  // BUG FIX: this used to be a plain "toolpage" with an inline
  // `maxWidth: 640` on top of that — a fixed reading-width box that left
  // most of any window wider than ~700px as dead, unused background next
  // to it (the same complaint as the Interfaces table, just without a
  // scrollbar to make it as visually obvious). "fluid" (styles.css) drops
  // the width cap so the page tracks the real window width, and the field
  // grids below switch from a fixed 2-column layout to `auto-fit`, which
  // adds MORE columns as the window gets wider instead of leaving the
  // extra space empty — a wide window shows 4 fields per row instead of
  // 2, a narrow one still wraps down to 1.
  return (
    <div className="toolpage fluid" style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
      <div className="toolpage-header">
        <h2>Settings</h2>
        <span className="sub">Saved to disk — reloaded automatically every time Net Pulse starts.</span>
      </div>

      {loadError && (
        <div className="tool-card danger">
          Couldn't load saved settings ({loadError}) — showing built-in defaults.
        </div>
      )}

      <section className="tool-section">
        <h3>Auto-refresh (background stale-route recheck)</h3>
        <div className="tool-section-desc">
          How often an already-resolved hop is quietly re-verified, and how many
          misses in a row before it's treated as a real route change and
          rediscovered. This is the passive background mechanism — separate
          from, and much slower by design than, pressing Force Recheck on a
          target.
        </div>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 260px))', gap: 12 }}>
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
          <div className="muted" style={{ fontSize: 11.5, marginTop: 10 }}>
            Currently applied in the engine: {applied.windowSecs}s / {applied.threshold} misses.
          </div>
        )}
      </section>

      <section className="tool-section">
        <h3>New target defaults</h3>
        <div className="tool-section-desc">
          What a freshly opened "Add target" form starts pre-filled with.
          Targets already added keep whatever they were given at the time —
          changing this only affects new ones.
        </div>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 260px))', gap: 12 }}>
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
        <label className="cb" style={{ marginTop: 12 }}>
          <input type="checkbox" checked={!!values.defaultRaw} onChange={set('defaultRaw')} />
          Raw/privileged mode by default
        </label>
      </section>

      <section className="tool-section">
        <h3>Appearance</h3>
        <div style={{ display: 'flex', gap: 18 }}>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
            <input type="radio" name="theme" value="dark" checked={values.theme === 'dark'} onChange={set('theme')} /> Dark
          </label>
          <label style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
            <input type="radio" name="theme" value="light" checked={values.theme === 'light'} onChange={set('theme')} /> Light
          </label>
        </div>
      </section>

      {saved && (
        <div className={'tool-card ' + (saved.ok ? 'success' : 'danger')}>
          {saved.ok ? saved.message : `Couldn't save settings: ${saved.message}`}
        </div>
      )}

      <div style={{ display: 'flex', gap: 10, justifyContent: 'flex-end', marginTop: 4 }}>
        <button onClick={resetDefaults} disabled={saving}>Reset to defaults</button>
        <button onClick={save} disabled={saving} className="primary">{saving ? 'Saving…' : 'Save'}</button>
      </div>
    </div>
  )
}

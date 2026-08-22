// main.jsx — the only other file (besides tauri-bridge.js) that differs from
// the Electron-era frontend. Installing the bridge BEFORE importing App
// guarantees window.netpulse exists by the time App's module-level code runs
// (App.jsx references it lazily inside functions/effects, not at import
// time, but this ordering removes any doubt).
import './tauri-bridge.js'
import React from 'react'
import ReactDOM from 'react-dom/client'
import App from './App.jsx'
import './styles.css'

// Safety net for the blank-black-screen-after-sleep bug: if a resume-triggered
// render exception ever unmounts the tree, this catches it and renders a
// visibly recoverable message instead of leaving a bare root against the
// app's dark window background (#0e1116), which otherwise reads as a
// hang/crash with no feedback at all.
class ErrorBoundary extends React.Component {
  constructor(props) { super(props); this.state = { error: null } }
  static getDerivedStateFromError(error) { return { error } }
  render() {
    if (this.state.error) {
      return (
        <div style={{
          position: 'fixed', inset: 0, display: 'flex', flexDirection: 'column',
          alignItems: 'center', justifyContent: 'center', gap: 14, background: '#0e1116',
          color: '#e6edf3', font: '14px/1.5 sans-serif', padding: 24, textAlign: 'center',
        }}>
          <div style={{ fontSize: 16, fontWeight: 600 }}>Something went wrong</div>
          <div style={{ opacity: 0.7, maxWidth: 480 }}>{String(this.state.error?.message || this.state.error)}</div>
          <button onClick={() => window.location.reload()} style={{
            padding: '8px 18px', borderRadius: 8, border: '1px solid #30363d',
            background: '#21262d', color: '#e6edf3', cursor: 'pointer', font: 'inherit',
          }}>Reload</button>
        </div>
      )
    }
    return this.props.children
  }
}

// Safety net for ASYNC errors — the ErrorBoundary below only catches
// exceptions thrown during React's own render/commit cycle; it does NOT see
// an unhandled promise rejection (e.g. an async event handler that throws
// with no .catch() anywhere in its chain). That gap was real and had already
// bitten this app once: a click handler wired straight to an async function
// (`onClick={addTarget}`) meant any exception inside it — including this
// codebase's own capability-check code — became invisible unless DevTools
// happened to be open AND its message-level filter happened not to be hiding
// it. Individually adding .catch() to every async call site is good practice
// but not a guarantee against the NEXT one; this is the actual backstop.
window.addEventListener('unhandledrejection', (event) => {
  console.error('[NetPulse] Unhandled promise rejection:', event.reason)
  showGlobalErrorBanner(String(event.reason && event.reason.message || event.reason))
})
window.addEventListener('error', (event) => {
  console.error('[NetPulse] Uncaught error:', event.error || event.message)
  showGlobalErrorBanner(String((event.error && event.error.message) || event.message))
})
let bannerEl = null
let bannerTimer = null
function showGlobalErrorBanner(text) {
  if (!bannerEl) {
    bannerEl = document.createElement('div')
    bannerEl.style.cssText =
      'position:fixed;bottom:0;left:0;right:0;z-index:99999;background:#7f1d1d;color:#fff;' +
      'font:13px/1.5 -apple-system,sans-serif;padding:10px 16px;white-space:pre-wrap;' +
      'max-height:35vh;overflow:auto;box-shadow:0 -2px 12px rgba(0,0,0,.4);'
    const close = document.createElement('button')
    close.textContent = '✕ dismiss'
    close.style.cssText = 'float:right;background:none;border:none;color:#fff;cursor:pointer;font:inherit;opacity:.8'
    close.onclick = () => { bannerEl.remove(); bannerEl = null }
    bannerEl.appendChild(close)
    const msgEl = document.createElement('div')
    msgEl.className = 'banner-msg'
    bannerEl.appendChild(msgEl)
    document.body.appendChild(bannerEl)
  }
  const msgEl = bannerEl.querySelector('.banner-msg')
  msgEl.textContent = 'An unexpected error occurred: ' + text
  clearTimeout(bannerTimer)
  bannerTimer = setTimeout(() => { if (bannerEl) { bannerEl.remove(); bannerEl = null } }, 20000)
}

ReactDOM.createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <ErrorBoundary>
      <App />
    </ErrorBoundary>
  </React.StrictMode>,
)

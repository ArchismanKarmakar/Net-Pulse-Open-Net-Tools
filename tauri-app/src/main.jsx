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

ReactDOM.createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <ErrorBoundary>
      <App />
    </ErrorBoundary>
  </React.StrictMode>,
)

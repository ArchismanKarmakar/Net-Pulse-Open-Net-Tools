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

ReactDOM.createRoot(document.getElementById('root')).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
)

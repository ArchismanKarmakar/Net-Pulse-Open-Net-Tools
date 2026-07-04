# Third-Party Notices

NetPulse is built on the following open-source components. This app does not
modify any of their source; it links/bundles them as-is per their licenses.

## Runtime dependencies

| Component | License | Role |
|---|---|---|
| [Electron](https://www.electronjs.org/) | MIT | Desktop app shell |
| [Node.js](https://nodejs.org/) | MIT | JS runtime embedded in Electron |
| [node-addon-api](https://github.com/nodejs/node-addon-api) | MIT | C++ wrapper for Node-API, used by `napi/` |
| [CMake.js](https://github.com/cmake-js/cmake-js) | MIT | Builds the native addon, Node-version-independent |
| [React](https://react.dev/) | MIT | UI library |
| [Recharts](https://recharts.org/) | MIT | Charting library (the RTT/latency graph) |
| [Tailwind CSS](https://tailwindcss.com/) | MIT | Utility CSS framework, compiled at build time (no runtime dependency) |
| [Vite](https://vitejs.dev/) | MIT | UI build tool |

## Data sources (optional, outbound HTTPS only)

The BGP/routing lookup panel queries public, free, no-auth-required APIs at
runtime, only when a hop's IP is inspected:
- [RIPEstat Data API](https://stat.ripe.net/) (RIPE NCC) — ASN/prefix/RPKI/geo/whois
- Public looking-glass links to [bgp.he.net](https://bgp.he.net), [PeeringDB](https://www.peeringdb.com/)

These are informational only; NetPulse does not redistribute their data, and
the app functions (probing, graphs, exports) fully without network access to
them — only the BGP drawer requires connectivity.

## Fonts

No web fonts are bundled or fetched. The UI uses the OS's installed system
font stack (sans-serif for UI text, monospace for numeric/IP data), avoiding
both font-licensing complexity and any dependency on a font CDN — consistent
with the app's Content-Security-Policy (`connect-src 'self' https:`).

## Attribution note

"PingPlotter" and "mtr" are referenced in documentation only for behavioral
comparison; NetPulse is an independent project and is not affiliated with
either. Their names are trademarks of their respective owners.

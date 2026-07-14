# Third-Party Notices

NetPulse is built on the following open-source components. This app does not
modify any of their source; it links/bundles them as-is per their licenses.

## Runtime dependencies

| Component | License | Role |
|---|---|---|
| [Tauri](https://tauri.app/) | MIT/Apache-2.0 | Desktop app shell (Rust host + OS WebView) |
| [Rust](https://www.rust-lang.org/) | MIT/Apache-2.0 | Language/runtime for the desktop host |
| [cxx](https://cxx.rs/) | MIT/Apache-2.0 | Safe C++↔Rust FFI bridge, statically links `core/` into the Rust binary |
| [tokio](https://tokio.rs/) | MIT | Async runtime, used by the port scanner |
| [hickory-resolver](https://github.com/hickory-dns/hickory-dns) | MIT/Apache-2.0 | DNS/reverse-DNS resolution |
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

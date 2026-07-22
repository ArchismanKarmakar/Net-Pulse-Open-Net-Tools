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
| [Motion](https://motion.dev/) | MIT | Drag-to-reorder for the target list (`Reorder.Group`/`Reorder.Item`) |
| [html-to-image](https://github.com/bubkoo/html-to-image) | MIT | Renders the hop table to a PNG for the "Traceroute PNG" export |
| [SheetJS (xlsx)](https://sheetjs.com/) | Apache-2.0 | Builds the `.xlsx` workbook for Excel exports. Installed from SheetJS's own CDN (`cdn.sheetjs.com`), not the npm registry — the npm-published version is permanently frozen on an old release with two known vulnerabilities (SheetJS stopped publishing fixed builds to npm after a policy dispute); see `tauri-app/.npmrc` for why this is the one dependency in this project installed from a non-registry URL |
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

## Icons

A handful of [Material Symbols](https://fonts.google.com/icons) (Google,
Apache-2.0) are bundled as inline SVG React components
(`tauri-app/src/components/icons/MaterialIcons.jsx`) for the per-target
export buttons — not as an icon font or an npm icon package, for the same
reason noted under Fonts above: only 5 specific glyphs are used, downloaded
once from Google's [material-design-icons](https://github.com/google/material-design-icons)
repository and bundled directly.

## Attribution note

"PingPlotter" and "mtr" are referenced in documentation only for behavioral
comparison; NetPulse is an independent project and is not affiliated with
either. Their names are trademarks of their respective owners.

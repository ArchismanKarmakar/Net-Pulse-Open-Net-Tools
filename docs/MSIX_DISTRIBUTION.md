# MSIX packaging for Microsoft Store distribution

Status as of this writing: **Option 1 (hand-rolled) is implemented and is
the one to use. Option 2 (community tool) is implemented but deliberately
NOT wired up for normal use yet** — see "Current status" below before doing
anything with either.

## Why this isn't just another Tauri bundle target

Tauri does not build MSIX packages at all. Its Windows bundler only
produces `.exe` (NSIS) and `.msi` (WiX) installers — `tauri.conf.json`'s
`bundle.targets` has no `"msix"` option to add. Tauri's own docs
acknowledge this directly: their Microsoft Store guidance is "create a
Microsoft Store application that only links to the unpacked application,"
i.e. a Store listing that just points at a regular NSIS/MSI installer
hosted elsewhere — not a real, sandboxed MSIX package.

That's a legitimate path if all you want is a Store *listing*, but it
isn't a real MSIX (no separate app identity, no Store-managed auto-update,
none of the properties an actual MSIX submission gets reviewed against).
Getting a genuine `.msix` file means either building it by hand with
Microsoft's own packaging tools, or using a third-party tool that automates
that. This project has both, see below.

## Option 1 — hand-rolled with the Windows SDK only (ACTIVE)

**Workflow:** `.github/workflows/msix-build.yml` (manual `workflow_dispatch`
only, like `tauri-canary-build.yml` — not on every push).

**How it works**, step by step:
1. Build the CLI sidecar (same step as every other workflow in this repo).
2. `npx tauri build --no-bundle` — produces the raw, unpacked
   `netpulse.exe` (+ any DLLs cargo drops next to it) without going
   through Tauri's NSIS/MSI bundler at all.
3. Generate the four PNG tile images the Store manifest schema requires
   (`Square44x44Logo.png`, `Square150x150Logo.png`, `Wide310x150Logo.png`,
   `StoreLogo.png`) from the existing app icon — see
   `tauri-app/src-tauri/windows/msix/generate-assets.py`. These are a
   *different* asset set than Tauri's own icon pipeline produces (that one
   targets `.ico`/`.icns` + NSIS/AppImage/dmg sizes; none of it matches
   what an MSIX manifest asks for).
4. Render `tauri-app/src-tauri/windows/msix/AppxManifest.xml.template`
   with `sed`, substituting the package identity, version, publisher, and
   display strings.
5. Stage `netpulse.exe`, the renamed CLI sidecar (`npulse.exe`, dropping
   the target-triple suffix — mirroring exactly what the NSIS installer's
   `externalBin` handling already does, see `windows/hooks.nsh`), the
   rendered manifest, and the generated `Assets/` folder into one
   directory.
6. `makeappx.exe pack` that directory into a `.msix` — this tool ships
   with the Windows SDK that's already preinstalled on the `windows-latest`
   GitHub runner image, found by searching under `Windows Kits\10\bin\*\x64\`
   rather than a hardcoded SDK version number (which would break the
   moment GitHub updates the runner image — the exact class of "path baked
   in, breaks later" bug this project already hit once with a stale
   `CMakeCache.txt`).
7. Sign the `.msix` with a **throwaway, CI-generated self-signed test
   certificate** (`signtool.exe`, also from the Windows SDK) — this makes
   the artifact installable locally via `Add-AppxPackage` for sideload
   testing. It is explicitly *not* the signature real users get; see
   "Submitting to the Store for real" below.
8. Upload the `.msix` as a workflow artifact.

**Zero third-party dependencies** — everything used
(`makeappx.exe`/`signtool.exe`/PowerShell's `New-SelfSignedCertificate`)
ships with Windows/the Windows SDK already installed on the runner.

**What's verified vs. not**: the `AppxManifest.xml.template` is checked
well-formed XML (both Python's `xml.dom.minidom` and `xmllint` were run
against a rendered copy) and closely mirrors Microsoft's own published
"manually convert a desktop app to MSIX" example rather than being
invented from scratch. `generate-assets.py` was actually run, producing
correctly-sized PNGs (verified by opening each one and checking pixel
dimensions). Every PowerShell step in the workflow was parsed (not
executed — `New-SelfSignedCertificate` is Windows-only) with a real
PowerShell 7 interpreter to catch syntax errors before this was ever
tested in real CI. What could **not** be verified in this environment:
`makeappx pack` actually running and the resulting `.msix` actually
installing — both `makeappx.exe` and `signtool.exe` are Windows-only with
no Linux-runnable equivalent. **The first real run of `msix-build.yml` on
a Windows runner is the actual end-to-end test.** Its output should be
downloaded and sideloaded (`Add-AppxPackage -Path netpulse.msix`) on a
real Windows machine and smoke-tested before trusting it for anything
further.

## Option 2 — `@choochmeque/tauri-windows-bundle` (SCAFFOLDED, NOT USED)

**Workflow:** `.github/workflows/msix-build-community-tool.yml` — manual
`workflow_dispatch` only, and additionally gated behind typing an exact
confirmation phrase, specifically to make it hard to trigger by accident.

This is a purpose-built npm package
(`github.com/Choochmeque/tauri-windows-bundle`) that automates most of
what Option 1 does by hand: it reads `tauri.conf.json` directly, generates
the manifest and tile assets itself, and supports both x64 and arm64 in
one pass, plus a growing library of optional Windows integrations (file
associations, protocol handlers, startup tasks, App Execution Alias, and
more) that Option 1 would otherwise mean hand-editing
`AppxManifest.xml.template` for, one at a time, as this project's Windows
feature surface grows.

**Why it isn't the default**: as of this writing it's genuinely new
(created January 2026, latest published version `0.2.0` as of September
2026) from a single maintainer, with no long track record. Pulling it into
a release pipeline is exactly the kind of "trust a third party without
verifying it" move that caused this project's real EnVar NSIS-plugin CI
failure (see `CHANGELOG.md` — the fix for that is the direct reason this
document exists in this form, flagging trust assumptions explicitly rather
than burying them in a comment nobody re-reads).

It's kept implemented, not deleted, so that switching later is a config
decision, not a rewrite. **Do not enable it for real use until:**
- it's actually been run at least once and its output sideloaded/smoke-
  tested on a real Windows machine, the same bar Option 1 hasn't fully
  cleared yet either, and
- either Option 1's manual-manifest maintenance becomes a real burden as
  more Windows integrations are added, or Option 2 has enough of a track
  record (more releases, other projects depending on it, etc.) to trust it
  in a release pipeline.

It deliberately does not modify `package.json`/`Cargo.toml` — its `init`
step runs fresh, inside that workflow's own checkout, only when someone
manually triggers it. Nothing about the normal build (`npm ci`,
`cargo build`, `tauri-ci.yml`, `tauri-release.yml`) is affected by this
file existing in the repo.

## Submitting to the Store for real (either option)

Both options currently use **placeholder identity values**
(`ArchismanKarmakar.NetPulseOpenNetTools` / `CN=00000000-...`) — these work
for local build/sideload testing but **will be rejected by Store
ingestion** if submitted as-is. Before a real submission:

1. Reserve the app name in
   [Partner Center](https://partner.microsoft.com/dashboard) (requires a
   registered developer account — a one-time fee applies for individual
   accounts).
2. Partner Center's "App identity" page for the reserved app gives you the
   **exact** `Package/Identity/Name` and `Package/Identity/Publisher`
   strings to use — these are account-specific and cannot be guessed or
   invented; the publisher string in particular is tied to your Partner
   Center account, not a certificate you generate yourself.
3. Re-run `msix-build.yml` with those exact values as the
   `package_identity_name`/`publisher` workflow inputs.
4. Upload the resulting `.msix` in Partner Center's submission flow.
   **Microsoft re-signs the package with its own certificate during
   certification** — the self-signed test certificate this workflow
   generates never reaches end users and does not need to match anything
   Partner Center issues.
5. Partner Center's own certification process (the Windows App
   Certification Kit checks, content policy review, etc.) is a separate
   gate this repo's CI has no way to pre-check — expect the first real
   submission to surface issues no amount of local testing catches.

## A known, accepted limitation: `runFullTrust`

NetPulse is a native Win32 binary (a Tauri app, not a UWP/sandboxed app),
so its MSIX package must declare the `runFullTrust` restricted capability
— there's no way around this for what NetPulse actually does (raw ICMP
sockets, firewall rule management via `netsh`/COM APIs, raw TCP, etc., see
`windows/hooks.nsh` and the core engine). A true UWP/AppContainer port is
not feasible: the AppContainer sandbox blocks raw ICMP sockets, blocks
spawning `netsh` or calling the firewall COM APIs, and blocks raw TCP
outright — all of which are core to what this app does, not incidental
features that could be trimmed away.

This is **not** a Store-review penalty. `runFullTrust` MSIX is Microsoft's
own sanctioned distribution path for exactly this class of app — Discord,
OBS Studio, VS Code, and Spotify all ship to the Store this way. It only
excludes a small number of narrow programs that don't apply to NetPulse
(the Kids/Education catalog, certain enterprise-sandboxing certifications).
Conclusion: **keep `runFullTrust` as-is; do not attempt a UWP rewrite.**

## Publisher naming: `ArchismanCoder` vs. `Archisman Karmakar`

Both can be used — they answer different fields and don't conflict:

- **`Package/Identity/Publisher`** (the `CN=...` string in
  `AppxManifest.xml.template` / this workflow's `publisher` input) is not a
  free choice at all. It must be the *exact* string Partner Center issues
  once the app identity is reserved, tied to the Partner Center account —
  whichever name you registered that account under. This field is
  effectively invisible to end users.
- **`Properties/PublisherDisplayName`** (the human-readable, Store-facing
  name shown to shoppers, currently rendered from this workflow as
  "Archisman Karmakar") is free text and can be set to `ArchismanCoder` if
  that's the byline wanted on the Store listing.
- **`tauri.conf.json`'s `bundle.publisher`** (`"Archisman Karmakar"`) is
  Tauri's own NSIS/MSI installer metadata (shown in Windows'
  Add/Remove Programs, the installer's publisher field, etc.) — entirely
  separate from the MSIX manifest and does not need to match it.

So: if the Store listing should read "ArchismanCoder", change
`PUBLISHER_DISPLAY_NAME` in `msix-build.yml`'s "Render AppxManifest.xml
from template" step to `ArchismanCoder`, leave `tauri.conf.json`'s
`bundle.publisher` untouched (that's the separate NSIS/MSI installer
identity), and set the workflow's `publisher` input to whatever exact
`CN=...` string Partner Center issues once the app name is reserved —
that string is fixed by the account, not a choice between the two names.

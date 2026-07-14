# Code Signing Guide (future implementation)

This is a practical, step-by-step guide to actually getting Net Pulse — Open
Net Tools **signed and trusted** by Windows SmartScreen, macOS Gatekeeper, and
antivirus/VirusTotal engines. `SECURITY.md` already covers the *hardening*
side (how the app minimizes attack surface); this document is specifically
about the **signing/reputation** side, which is a separate concern — trust in
OS/AV terms comes from a verifiable signing identity plus accumulated
reputation, not from source code quality alone. Nothing here is wired up yet
in this repository beyond the `tauri-action` plumbing in
`.github/workflows/tauri-release.yml` (which already reads signing config from
repository secrets, see below) — this is the runbook for when a certificate is
actually acquired.

## The free path — start here (this is what you asked for)

Since Net Pulse — Open Net Tools is AGPL-3.0-or-later and publicly on GitHub,
it genuinely qualifies for **free, real, Trusted-Root code signing** for
Windows — not a workaround, an actual OV certificate from a CA Windows
already trusts. Researched fresh (not from stale general knowledge, since
pricing/eligibility for these programs changes):

### Windows — SignPath Foundation (real, $0, use this)

[**SignPath Foundation**](https://signpath.org/) is a nonprofit that issues
free OV code-signing certificates to qualifying open-source projects, backed
by **Sectigo** — a CA in the Microsoft Trusted Root Program, so it clears
SmartScreen's "Unknown Publisher" warning for real, not a workaround. The
private key lives on SignPath's HSM; you never handle it, which is also just
better security practice. Signing happens automatically in CI (GitHub
Actions) — you never sign locally.

**Eligibility (confirmed current criteria):**
- OSI-approved license — AGPL-3.0-or-later qualifies.
- Public repository with a working, documented build pipeline (this
  project's `.github/workflows/` already has one).
- **The project must already have a released build** — a fresh repo with no
  release history isn't eligible yet. **Practical implication: cut a real
  GitHub Release first** (even unsigned), then apply.
- Adherence to their Code of Conduct (not malicious/adware — trivially true
  here).

**How to apply:** via [signpath.io/solutions/open-source-community](https://signpath.io/solutions/open-source-community).
Expect the application to ask for: what gets signed (the NSIS `.exe`/`.msi`
from `tauri-app/`), your build/CI process, and — for a single-maintainer
project — you'll list yourself as author/reviewer/approver. Processing is
typically days to a few weeks.

**The one tradeoff:** the publisher name shown in the Windows install dialog
and SmartScreen will read **"SignPath Foundation"**, not "Archisman
Karmakar" or "Net Pulse" — because the certificate is issued to the
Foundation, which vouches for your repo, not to you personally. For almost
all OSS projects (including this one) that's a completely fine trade for a
real, trusted, $0 signature. If you specifically want *your own name* on the
certificate, that requires a paid OV/EV cert from a CA directly (see below).

**Backup option if SignPath's queue is long:** [OSSign](https://ossign.org/)
offers the same kind of free OSS signing with similar eligibility criteria
(OSI license, 6+ months of project activity, verifiable build pipeline) — as
of this writing their applications are temporarily paused due to backlog, so
check current status before relying on it as your primary path.

### A geography note if you're not in the US/Canada/EU/UK

**Azure Trusted Signing** (Microsoft's own ~$9.99/month service, the
cheapest *paid* option if SignPath's free path doesn't fit) restricts its
publicly-trusted tier, **as of February 2026, to organizations in the
US/Canada/EU/UK and individual developers in the US/Canada only.** If you're
applying as an individual outside those regions, this option may not
actually be available to you regardless of budget — SignPath Foundation
doesn't have that restriction (it's gated on the *project* being genuine
OSS, not the maintainer's country), which makes it the more relevant free
option either way.

### macOS — there is no free path; $99/year is the real floor

Unlike Windows, Apple doesn't have an OSS-sponsored free-signing program
equivalent to SignPath. A **Developer ID** certificate that clears
Gatekeeper for other people's Macs requires an **Apple Developer Program
membership — $99/year**, paid directly to Apple, regardless of the app being
open source. There's no legitimate way around this specific cost; be
skeptical of anything claiming otherwise. Two partial options if $99/year
isn't viable right now:
- **Ad-hoc self-signing** (`codesign --sign -`, no Apple account needed) —
  lets *you* run the unsigned build locally without the full Gatekeeper
  quarantine dialog, but does **not** clear Gatekeeper for anyone you
  distribute the app to; not a real distribution solution.
- Document clearly (README + release notes) that macOS users will see a
  Gatekeeper warning and need to right-click → Open the first time — an
  honest, common practice for unsigned-but-legitimate OSS macOS apps.

### Linux — no OS-level gatekeeper, but sign anyway

There's no SmartScreen/Gatekeeper equivalent, but two genuinely free
mechanisms build real trust: a detached **GPG signature** + published public
key alongside each AppImage release (the long-standing convention), and
increasingly, **[Sigstore](https://www.sigstore.dev/)/`cosign`** — free,
keyless signing tied to your GitHub OIDC identity, now the de facto standard
for npm/PyPI/container provenance. It doesn't suppress any warning dialog
(nothing checks it automatically the way SmartScreen checks Authenticode),
but it's a real, free, verifiable provenance signal worth adding regardless.

---

## Why an unsigned build gets flagged, regardless of code quality

An **unsigned** binary that (a) is a new/unknown publisher to SmartScreen's
reputation database, and (b) opens raw sockets and includes a port scanner —
both legitimate diagnostic features, but also exactly the shape of behavior
heuristic AV engines watch for — will draw SmartScreen warnings and a handful
of heuristic VirusTotal detections **no matter how clean the source is**.
This is expected and is not a verdict on the code; it is a statement about
lacking a trusted signing identity yet. Signing is the fix, not more code
changes.

---

## Windows: Authenticode signing

### 1. Choose a certificate type

| Type | Cost (typical) | SmartScreen behavior |
|---|---|---|
| **OV** (Organization Validation) | ~$100–300/yr | Reputation must **accrue over time** via downloads — early releases still show a SmartScreen prompt (though a milder one than fully unsigned) until enough users have run it. |
| **EV** (Extended Validation) | ~$300–700/yr, requires a hardware token/HSM | **Instant** SmartScreen reputation — no warm-up period. Strongly recommended if the budget allows, specifically because it removes the "why does this trip a warning on day one" support burden. |

Recognized CAs: DigiCert, Sectigo (Comodo), GlobalSign, SSL.com. EV certs are
issued to a **verified legal entity** (a business, not typically an
individual) and are delivered on a hardware token or cloud HSM (e.g. Azure
Trusted Signing, DigiCert KeyLocker) rather than a plain exportable `.pfx` —
this is a CA/Microsoft policy requirement for EV, not a project choice.

### 2. Sign with the OV file-based flow (simplest)

Set the certificate as repository secrets consumed by
`.github/workflows/tauri-release.yml`'s `WINDOWS_CERTIFICATE` /
`WINDOWS_CERTIFICATE_PASSWORD` env vars (base64-encode the `.pfx` for
`WINDOWS_CERTIFICATE`) — `tauri-apps/tauri-action` picks these up
automatically, no workflow change needed.

### 3. Sign with an EV cert (hardware token / cloud HSM)

EV certs can't be pointed to via a plain certificate secret (there's no
exportable private key file). Two practical paths:

- **Cloud HSM signing service** (recommended — no physical dongle to manage
  on a CI runner): Azure Trusted Signing or DigiCert KeyLocker both provide a
  `signtool`-compatible CLI/plugin. Tauri's bundler supports a
  [custom sign command](https://v2.tauri.app/distribute/sign/windows/) to
  invoke that CLI instead of the default signtool invocation.
- **Physical USB HSM token**: sign on the machine the token is plugged into,
  using the CA's own signing tool (usually a `signtool.exe`-compatible
  wrapper) as a manual post-build step, before uploading the release
  artifact.

### 4. Verify the signature

```powershell
signtool verify /pa "Net Pulse — Open Net Tools_0.9.1_x64-setup.exe"
```

`/pa` uses the default Authenticode policy (matches what Windows actually
checks). A clean run prints `Successfully verified`.

### 5. RFC 3161 timestamping

Tauri's Windows signing invokes `signtool` with a timestamp server
automatically when signing is configured. This embeds a trusted timestamp in
the signature so the binary **remains valid after the certificate itself
expires** — without it, every signed binary from a given cert silently
becomes "unsigned" the day the cert expires, which is a much worse trust
regression than never having signed at all.

---

## macOS: Developer ID + notarization

### 1. Prerequisites

- An **Apple Developer Program** account (paid, ~$99/yr).
- A **Developer ID Application** certificate, exported as `.p12`.
- An **app-specific password** for notarization (generate at
  [appleid.apple.com](https://appleid.apple.com) → Sign-In and Security →
  App-Specific Passwords) — never use the real Apple ID password here.

### 2. Sign + notarize

Set the following as repository secrets, consumed by
`.github/workflows/tauri-release.yml`'s `env:` block for the `tauri-action`
step (`APPLE_CERTIFICATE` is the base64-encoded `.p12`):

```
APPLE_CERTIFICATE=<base64 .p12>
APPLE_CERTIFICATE_PASSWORD=****
APPLE_ID=you@example.com
APPLE_PASSWORD=****            # app-specific password, not the real Apple ID password
APPLE_TEAM_ID=XXXXXXXXXX
```

`tauri-apps/tauri-action` submits the signed app to Apple's notary service,
polls for the result, and **staples** the notarization ticket to the app
automatically — no separate `xcrun notarytool` invocation needed, and no
workflow change beyond having the secrets set.

### 3. Why notarization specifically (not just signing)

**Gatekeeper blocks an app that is signed but not notarized** just as hard as
one that's fully unsigned — signing alone proves who built it, notarization
is Apple's own automated malware scan confirming they've checked it. Tauri's
bundler applies hardened runtime automatically for macOS bundles; notarization
just needs the secrets above to be present.

### 4. Verify

```bash
spctl -a -vv "/Applications/Net Pulse — Open Net Tools.app"
codesign --verify --deep --strict --verbose=2 "/Applications/Net Pulse — Open Net Tools.app"
```

A successful notarized app reports `source=Notarized Developer ID`.

---

## Linux: no central signing authority, but still build trust

There's no OS-level Gatekeeper/SmartScreen equivalent for a generic AppImage
or `.deb`, so the goal shifts from "pass a gate" to "let a user verify
provenance":

1. **Publish `SHA256SUMS`** alongside every release artifact.
2. **GPG-sign the checksums file** (`gpg --detach-sign --armor SHA256SUMS`)
   with a key whose fingerprint is published in the repo (e.g. in
   `SECURITY.md`) — this lets a user verify the release actually came from
   the maintainer, independent of any single distribution channel's trust.
3. If distributing via a package repository (a PPA, Flatpak/Flathub, or an
   AUR package) rather than raw downloads, that channel's own signing/review
   process (Flathub's build review, in particular) adds independent
   third-party trust on top.

---

## Building reputation over time (all platforms)

- **Keep the signing identity stable** — same certificate/Team ID, same
  `appId`/`productName`, release after release. Reputation systems
  (SmartScreen especially) key on exactly this; a new cert on every release
  resets the reputation clock to zero.
- **Distribute from one consistent, HTTPS origin** — GitHub Releases — with
  checksums, rather than mirrors of unknown provenance.
- **Never pack the native binary** (no UPX). Tauri's bundler does not pack —
  packers are one of the single biggest AV false-positive triggers, because
  legitimate compressors and malware droppers use the exact same technique to
  hide payload bytes from static scanners.

## Handling residual VirusTotal detections after signing

Even a properly signed, notarized build can draw 1–3 heuristic hits from
smaller AV engines — this is normal for any tool that opens raw sockets or
includes a port scanner, signed or not.

1. **Always re-scan the signed artifact**, never the unsigned one — an
   unsigned scan result is not representative of what a user will actually
   run.
2. **File false-positive reports** with the specific flagging vendors (most
   have a web form for this, including Microsoft Defender's own submission
   portal). Link the public GitHub source and the exact release tag/commit —
   vendors whitelist known open-source tools meaningfully faster when the
   build is reproducible and auditable end-to-end.
3. **Give it time.** Reputation propagation across AV vendor databases is not
   instant — expect days to a few weeks after the first signed, notarized
   release before detections fully clear.

## Checklist

- [ ] `tauri-app` builds cleanly (`npx tauri build`).
- [ ] Windows: OV or EV certificate acquired; `WINDOWS_CERTIFICATE`/
      `WINDOWS_CERTIFICATE_PASSWORD` secrets set (OV), or HSM sign command
      configured (EV).
- [ ] macOS: Developer ID cert + Apple Developer account; `APPLE_CERTIFICATE`,
      `APPLE_CERTIFICATE_PASSWORD`, `APPLE_ID`, `APPLE_PASSWORD`,
      `APPLE_TEAM_ID` secrets set.
- [ ] `tauri-release.yml` run produced signed artifacts for each OS.
- [ ] Signature verified (`signtool verify /pa`, `spctl -a -vv`).
- [ ] `SHA256SUMS` published and GPG-signed.
- [ ] Signed artifact scanned on VirusTotal; false-positive reports filed for
      any residual detections.

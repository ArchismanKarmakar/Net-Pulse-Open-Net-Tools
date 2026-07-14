# Code Signing Guide (future implementation)

This is a practical, step-by-step guide to actually getting Net Pulse — Open
Net Tools **signed and trusted** by Windows SmartScreen, macOS Gatekeeper, and
antivirus/VirusTotal engines. `SECURITY.md` already covers the *hardening*
side (how the app minimizes attack surface); this document is specifically
about the **signing/reputation** side, which is a separate concern — trust in
OS/AV terms comes from a verifiable signing identity plus accumulated
reputation, not from source code quality alone. Nothing here is wired up yet
in this repository beyond the electron-builder plumbing (which already reads
signing config from environment variables, see below) — this is the runbook
for when a certificate is actually acquired.

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

```powershell
$env:CSC_LINK = "C:\path\to\cert.pfx"
$env:CSC_KEY_PASSWORD = "********"
cd electron
npm run dist:win
```

`electron-builder` picks these up automatically — no config change needed.
The existing `electron/electron-builder.yml` already has `signtoolOptions`
wired (`publisherName`, `signingHashAlgorithms: ["sha256"]`,
`rfc3161TimeStampServer`); `publisherName` **must exactly match the
certificate's CN** (Common Name) or signing fails schema/identity checks.

### 3. Sign with an EV cert (hardware token / cloud HSM)

EV certs can't be pointed to via `CSC_LINK` (there's no exportable private
key file). Two practical paths:

- **Cloud HSM signing service** (recommended — no physical dongle to manage
  on a CI runner): Azure Trusted Signing or DigiCert KeyLocker both provide a
  `signtool`-compatible CLI/plugin. Configure electron-builder's
  [custom sign hook](https://www.electron.build/configuration/win#custom-sign)
  to invoke that CLI instead of the default signtool invocation.
- **Physical USB HSM token**: sign on the machine the token is plugged into,
  using the CA's own signing tool (usually a `signtool.exe`-compatible
  wrapper) either as electron-builder's sign hook or as a manual post-build
  step run on the same machine, before uploading the release artifact.

### 4. Verify the signature

```powershell
signtool verify /pa "release\Net Pulse — Open Net Tools Setup 0.8.1.exe"
```

`/pa` uses the default Authenticode policy (matches what Windows actually
checks). A clean run prints `Successfully verified`.

### 5. RFC 3161 timestamping (already configured)

`electron-builder.yml` already sets
`rfc3161TimeStampServer: http://timestamp.digicert.com`. This embeds a
trusted timestamp in the signature so the binary **remains valid after the
certificate itself expires** — without it, every signed binary from a given
cert silently becomes "unsigned" the day the cert expires, which is a much
worse trust regression than never having signed at all. Don't remove this.

---

## macOS: Developer ID + notarization

### 1. Prerequisites

- An **Apple Developer Program** account (paid, ~$99/yr).
- A **Developer ID Application** certificate, exported as `.p12`.
- An **app-specific password** for notarization (generate at
  [appleid.apple.com](https://appleid.apple.com) → Sign-In and Security →
  App-Specific Passwords) — never use the real Apple ID password here.

### 2. Sign + notarize

```bash
export CSC_LINK=DeveloperID.p12
export CSC_KEY_PASSWORD=****
export APPLE_ID=you@example.com
export APPLE_APP_SPECIFIC_PASSWORD=****
export APPLE_TEAM_ID=XXXXXXXXXX
```

Then set `mac.notarize: true` in `electron/electron-builder.yml` (currently
`false` — flip it once the above env vars are actually available) and run:

```bash
cd electron
npm run dist:mac
```

electron-builder submits the signed app to Apple's notary service, polls for
the result, and **staples** the notarization ticket to the app automatically
— no separate `xcrun notarytool` invocation needed.

### 3. Why notarization specifically (not just signing)

**Gatekeeper blocks an app that is signed but not notarized** just as hard as
one that's fully unsigned — signing alone proves who built it, notarization
is Apple's own automated malware scan confirming they've checked it.
Hardened runtime + the entitlements files already present in this repo
(`build/entitlements.mac.plist`, referenced by
`entitlements`/`entitlementsInherit` in `electron-builder.yml`) are required
inputs to notarization; they're already configured.

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
- **Never pack/obfuscate the binary** (no UPX). `electron-builder.yml`
  already sets `compression: normal` and never packs — packers are one of
  the single biggest AV false-positive triggers, because legitimate
  compressors and malware droppers use the exact same technique to hide
  payload bytes from static scanners.

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

- [ ] `web` built, `napi` built for the target Electron ABI.
- [ ] Windows: OV or EV certificate acquired; `CSC_LINK`/`CSC_KEY_PASSWORD`
      set (OV) or HSM sign hook configured (EV).
- [ ] macOS: Developer ID cert + Apple Developer account; notarization env
      vars set; `mac.notarize: true` flipped in `electron-builder.yml`.
- [ ] `npm run dist:win` / `dist:mac` / `dist:linux` produced signed
      artifacts.
- [ ] Signature verified (`signtool verify /pa`, `spctl -a -vv`).
- [ ] `SHA256SUMS` published and GPG-signed.
- [ ] Signed artifact scanned on VirusTotal; false-positive reports filed for
      any residual detections.

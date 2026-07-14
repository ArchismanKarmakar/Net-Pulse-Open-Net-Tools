# Privacy Policy

**Last updated: 2026-07-13**

Net Pulse — Open Net Tools ("Net Pulse", "the app") is a local, offline-first
network-diagnostics desktop application. This policy explains what data the
app touches, where it goes, and — just as importantly — what it does **not**
collect, because "we don't collect X" is a claim worth being specific about
rather than asserting in the abstract.

## Summary

- Net Pulse does **not** phone home. There is no telemetry, no analytics, no
  crash reporter, no update-check ping, and no account.
- Everything the app does — pinging, tracerouting, DNS lookups, port
  scanning — runs **locally on your machine**, initiated by **you**, against
  hosts **you** typed in.
- The only network requests the app itself ever makes on your behalf are the
  diagnostic probes you explicitly start (ICMP/ping, DNS, TCP connect scan)
  and, optionally, looking up BGP/ASN/routing metadata for an IP you're
  already viewing (RDAP/RIPEstat/similar public routing-registry APIs) — see
  [Third-party lookups](#third-party-lookups-bgpasn-metadata) below.
- No data this app generates is uploaded anywhere by the app itself. Any
  export (CSV, JSON, etc., if/when such a feature is used) is written to a
  file **you** choose, on **your** disk.

## What the app does locally

Net Pulse is an Electron desktop app with a C++ engine loaded in-process
(no server, no open port, no localhost API — see `ARCHITECTURE.md` /
`README.md`). When you add a target and click start, the engine:

- Resolves the hostname you entered via your OS's normal DNS resolution.
- Sends ICMP echo requests (or unprivileged datagram probes) to that host and
  the hops along the path to it, and records the replies (latency, loss,
  responding IP) **in memory**, for display in the app.
- Optionally performs a reverse-DNS (PTR) lookup on the IPs it discovers,
  again via your OS's normal DNS resolution, to show hostnames.

None of this is sent anywhere except to the hosts you're diagnosing — that
*is* the diagnostic, by definition (a ping has to reach the target to measure
it).

## What is stored, and where

- **In-memory only, by default.** Session state (hop statistics, latency
  history within the configured focus window) lives in the running process's
  memory and is discarded when you close the app or remove a target, unless
  you explicitly export it.
- **Local settings.** Your configured targets, intervals, and preferences are
  stored locally by Electron's standard mechanisms (e.g. `app.getPath('userData')`)
  on your own disk. They are never transmitted anywhere.
- **No cloud sync, no remote database, no server-side account of any kind.**

## Third-party lookups (BGP/ASN metadata)

For the per-hop ASN/BGP info panel, the app may query public routing-registry
lookup services (e.g. RDAP, RIPEstat, or similar public APIs) with the **IP
address you are already viewing** in order to show its owning network/ASN.
This is:

- **Optional and passive** — it only runs for IPs already visible in your own
  trace, never a bulk/background sweep.
- Sent directly from your machine to the third-party service's own API, over
  HTTPS — Net Pulse has no server in between and does not see or log this
  traffic itself.
- Subject to *that* service's own privacy policy, since it's a third party
  you're querying, not us. We don't control what they log on their end
  (typically just standard web-server access logs, per their own published
  policies).

The app's Content-Security-Policy restricts this to `https:` destinations
only (see `SECURITY.md`), and the feature can be avoided entirely by not
opening the ASN/BGP panel.

## Bundled tools (Ping / DNS Lookup / Port Scanner)

These tabs run using your OS's own network stack (spawned `ping`, DNS
resolution, TCP connect attempts), initiated only when you explicitly use
them, against hosts you specify. **Use the port scanner only against hosts
you own or are authorized to test** — this is a legal/ethical requirement on
you as the operator, not a data-collection concern of the app itself.

## What we do not do

- We do not collect usage analytics, telemetry, or crash reports.
- We do not track you, fingerprint your device, or create any user profile.
- We do not sell, share, or transmit your data to any third party, because we
  do not collect any data to begin with.
- We do not require an account, sign-in, or internet connection to use the
  core diagnostic features (DNS-dependent hostname resolution obviously
  needs a working network — the point above is there's no *account*).
- The app does not auto-update by silently phoning a server in the
  background; you obtain new releases from GitHub Releases yourself.

## Open source

Net Pulse — Open Net Tools is licensed AGPL-3.0-or-later and its full source
is public. If you want ground truth beyond this document, the source is the
actual authority — in particular `ARCHITECTURE.md` (engine internals) and
`SECURITY.md` (hardening details) describe exactly what the app does and
does not touch over the network.

## Changes to this policy

If this policy changes in a future release, the date at the top will be
updated and the change will be visible in the project's Git history, same as
any other source change — there is no separate, hidden "policy update"
mechanism.

## Contact

For privacy questions or to report a concern, open an issue on the GitHub
repository, or use the private security-advisory process described in
[`SECURITY.md`](SECURITY.md) if the concern is a vulnerability rather than a
policy question.

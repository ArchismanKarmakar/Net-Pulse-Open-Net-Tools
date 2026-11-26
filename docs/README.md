# Net Pulse 2.1.11

Continuous ping and traceroute for many targets at once, per-hop latency and
loss, IPv4 and IPv6, ASN and network names per hop, alerts and exports.
Free for personal and business use. Feature tour:
<https://netpulse.archismancoder.com/>

2.x is published as ready-to-run builds under the
[NetPulse Terms of Use](TERMS.md); its source code is not published. The
source in this repository is Net Pulse 1.x (AGPL-3.0).

## Download

From the [2.1.11 release](https://github.com/ArchismanKarmakar/Net-Pulse-Open-Net-Tools/releases/tag/2.1.11):

| System | File |
|---|---|
| Windows 10 / 11 (x64) | `…_x64-setup.exe` |
| macOS 10.15+, Apple silicon | `…_aarch64.dmg` |
| macOS 10.15+, Intel | `…_x64.dmg` |
| Linux (Debian, Ubuntu and derivatives) | `…_amd64.deb` |
| Linux (other distributions) | `…_amd64.AppImage` |
| Command line only (servers, no desktop) | `npulse-windows-latest.exe`, `npulse-macos-aarch64`, `npulse-macos-x86_64`, `npulse-ubuntu-latest` |

The `.app.tar.gz` and `.sig` files are used by the app's built-in updater;
you don't need them.

## Check the download

Every file is listed with its SHA-256 in `SHA256SUMS.txt` on the release.

```
# Windows (PowerShell)
Get-FileHash .\<file> -Algorithm SHA256
# macOS / Linux
shasum -a 256 <file>
```

The value must match the line for that file.

## First start

The builds are not yet signed with a code-signing certificate, so the system
warns on first start:

- **Windows:** SmartScreen shows "Windows protected your PC": choose
  *More info* > *Run anyway*.
- **macOS:** open the app once with right-click > *Open* > *Open* (or System
  Settings > Privacy & Security > *Open Anyway*).

Ping and traceroute need permission to send ICMP:

- **Windows:** the installer adds the Windows Firewall rules traceroute needs.
  If you cancelled the firewall prompt, use Settings > Windows Firewall in the
  app.
- **Linux:** the `.deb` grants the permission on install. The AppImage cannot
  hold it: use the app's *Grant permission…* button, run it with `sudo`, or
  untick **Raw** when adding a target (unprivileged ping).

## Coming from 1.x

2.1.11 installs over Net Pulse 1.x (same app): download and run the
installer for your system from the release above.

## Updates

The app checks <https://netpulse.archismancoder.com> for new versions and
asks before installing anything. What it sends and stores:
[Privacy](PRIVACY.md).

## Questions and problems

Open an issue in this repository, or write to
[contact@archismancoder.com](mailto:contact@archismancoder.com).
Security issues: see [`SECURITY.md`](../../SECURITY.md).

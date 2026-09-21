; Windows Firewall exceptions for Net Pulse, added at install time (elevated,
; via the installer) rather than requiring the app to run as Administrator
; on every launch. Confirmed necessary: on at least two test Windows PCs,
; ICMP traffic was found blocked by Windows Firewall, breaking traceroute
; entirely until the user manually added an "ICMP allow all" rule; the same
; class of block likely also explains ping/DNS/port-scanner failures if the
; app's outbound traffic in general is being firewalled at the program level.
;
; IMPORTANT -- corrected ICMP type scoping (previous version was wrong): the
; original rules used protocol=icmpv4:8,any / icmpv6:128,any for BOTH
; directions. Type 8 (v4) / 128 (v6) is Echo REQUEST only. Using that for the
; OUTBOUND rule was correct (we need to be able to send our own probes), but
; using it for the INBOUND rule was a real bug -- it only allowed OTHER hosts
; to ping netpulse.exe, not the actual replies our own probes need back
; (Echo Reply: type 0 v4 / 129 v6; Time Exceeded: type 11 v4 / 3 v6;
; Destination Unreachable: type 3 v4 / 1 v6). Traceroute specifically can't
; rely on Windows Firewall's normal "allow replies to my own outbound
; request" connection tracking either, because that tracking matches on the
; same peer replying -- but traceroute's replies come from a DIFFERENT host
; at every hop (each intermediate router), not from the final destination
; the packet was addressed to. That mismatch is exactly why an explicit,
; unrestricted-by-type inbound ICMP rule is required, and exactly why the
; original type-8-only inbound rule didn't fix the reported blocking even
; though it looked plausible. Fix: allow the whole icmpv4/icmpv6 protocol
; (no type restriction) in both directions -- still safely scoped to
; netpulse.exe specifically via program=, not a system-wide allow.
;
; netsh add rule failures are non-fatal: they only run at install time
; (already elevated, since the installer itself requires admin to write to
; Program Files), and a failure here (e.g. netsh missing on some locked-down
; system) must never abort the whole app installation over a firewall rule --
; so every ExecToLog result is ignored deliberately.
;
; ---------------------------------------------------------------------------
; CLI (npulse) PATH registration + sidecar-reinstall fix, added alongside the
; firewall rules above.
;
; UNVERIFIED IN CI: everything below is written to match NSIS's and the
; EnVar plugin's documented behavior as closely as possible, but this
; project's CI has no Windows NSIS build/install cycle that actually runs an
; installer end-to-end (tauri-ci.yml's "tauri build" step packages but never
; installs), so this hasn't been exercised by a real install the way the
; netpulse_core engine and the CLI binary itself have been (see CHANGES.md's
; verification section). Flagging this plainly rather than presenting it as
; tested.
;
; EnVar is NSIS's standard, purpose-built plugin for editing PATH correctly
; (handles the ";"-joining and, per its own documentation, skips adding a
; path that's already present) -- see https://nsis.sourceforge.io/EnVar_plug-in.
; Tauri's bundled NSIS template ships common plugins including EnVar for its
; own installer features, so no extra plugin-installation step should be
; needed, but this specific assumption is exactly the "unverified" part
; above.
;
; HKCU, not HKLM, deliberately: (1) doesn't require a second elevation
; consideration beyond what installing to Program Files already needs, (2) a
; real, still-open NSIS bug (https://sourceforge.net/p/nsis/bugs/1247/) means
; EnVar::SetHKLM can corrupt a long HKLM PATH on some Windows configurations
; -- HKCU doesn't hit that path in the plugin, and a per-user PATH entry is
; the right scope for a CLI tool anyway (no reason to touch every user's
; PATH on a shared machine for one user's install).
!macro NSIS_HOOK_PREINSTALL
  ; A real, documented Tauri/NSIS gotcha for externalBin sidecars
  ; specifically (github.com/tauri-apps/tauri/issues/15134): a sidecar exe
  ; commonly has no Windows version resource, which can make NSIS's file-
  ; replacement logic skip overwriting it on a same-version reinstall/
  ; upgrade -- and separately, Windows can never overwrite an exe that's
  ; still running at all, version resource or not. Killing it first,
  ; unconditionally, makes both cases safe. Best-effort, same as the
  ; firewall rules below: npulse.exe not currently running (the
  ; overwhelmingly common case -- it's a short-lived CLI invocation, not a
  ; background service) is not an error, so this result is never checked.
  nsExec::ExecToLog 'taskkill /F /IM npulse.exe'
  Pop $0
!macroend

!macro NSIS_HOOK_POSTINSTALL
  ; Allow the full ICMPv4 protocol (Echo Request/Reply, Time Exceeded,
  ; Destination Unreachable, etc.) to/from netpulse.exe specifically --
  ; scoped by program=, not a blanket system-wide ICMP allow.
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Net Pulse (ICMPv4-In)" dir=in action=allow program="$INSTDIR\netpulse.exe" protocol=icmpv4 enable=yes'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Net Pulse (ICMPv4-Out)" dir=out action=allow program="$INSTDIR\netpulse.exe" protocol=icmpv4 enable=yes'
  Pop $0

  ; Same for ICMPv6 -- separate rule since Windows Firewall treats
  ; icmpv4/icmpv6 as distinct protocol keywords.
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Net Pulse (ICMPv6-In)" dir=in action=allow program="$INSTDIR\netpulse.exe" protocol=icmpv6 enable=yes'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Net Pulse (ICMPv6-Out)" dir=out action=allow program="$INSTDIR\netpulse.exe" protocol=icmpv6 enable=yes'
  Pop $0

  ; General program-level allow for TCP/UDP (DNS lookups, port scanner,
  ; forward/reverse DNS via hickory-resolver) -- narrower than "any", scoped
  ; to this program only, both directions since the port scanner and other
  ; tools are pure outbound but DNS responses need the reply path allowed too.
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Net Pulse (App)" dir=in action=allow program="$INSTDIR\netpulse.exe" enable=yes'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall add rule name="Net Pulse (App Out)" dir=out action=allow program="$INSTDIR\netpulse.exe" enable=yes'
  Pop $0

  ; Tauri's externalBin bundling (tauri.conf.json's bundle.externalBin,
  ; "binaries/npulse") copies the sidecar into $INSTDIR alongside
  ; netpulse.exe, renamed to drop the target-triple suffix -- so the
  ; installed CLI is $INSTDIR\npulse.exe. Add $INSTDIR to the CURRENT
  ; USER's PATH so `npulse`/`ping`/`mtr`/etc. (see cli/CLI.md's "Standard
  ; commands" section for the full alias list) work from any shell without
  ; a full path, matching the "run through the OS shell like a native
  ; tool" goal -- Windows Terminal and PowerShell 7 both pick up a PATH
  ; change made this way in any NEW shell they open after install (an
  ; already-open shell needs to be restarted, same as any other PATH
  ; change on Windows).
  EnVar::SetHKCU
  EnVar::AddValue "Path" "$INSTDIR"
  Pop $0
!macroend

!macro NSIS_HOOK_POSTUNINSTALL
  ; Remove by name -- matches every rule added above regardless of direction,
  ; since `netsh ... delete rule name="X"` removes all rules with that exact
  ; name (both dir=in and dir=out entries share unique names above, so no
  ; wildcard needed).
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Net Pulse (ICMPv4-In)"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Net Pulse (ICMPv4-Out)"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Net Pulse (ICMPv6-In)"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Net Pulse (ICMPv6-Out)"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Net Pulse (App)"'
  Pop $0
  nsExec::ExecToLog 'netsh advfirewall firewall delete rule name="Net Pulse (App Out)"'
  Pop $0

  ; Undo the PATH addition above -- same HKCU scope, same EnVar plugin.
  EnVar::SetHKCU
  EnVar::DeleteValue "Path" "$INSTDIR"
  Pop $0
!macroend

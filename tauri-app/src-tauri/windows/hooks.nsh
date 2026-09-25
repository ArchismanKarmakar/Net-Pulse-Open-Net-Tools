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
; BUG FIX (real CI failure, "windows build failed again" -- build installer
; (windows-latest)): this used to call the third-party EnVar plugin
; (EnVar::SetHKCU / EnVar::AddValue / EnVar::DeleteValue) to edit the PATH.
; That failed makensis in CI with:
;   Plugin not found, cannot call EnVar::SetHKCU
;   Error in macro NSIS_HOOK_POSTINSTALL on macroline 36
; The comment that used to sit here said Tauri's bundled NSIS template
; "ships common plugins including EnVar" -- that assumption was wrong and is
; what caused this. Confirmed two ways: (1) tauri-action/tauri-bundler only
; downloads NSIS itself plus Tauri's own nsis_tauri_utils plugin (visible in
; the CI log a few lines above this error, downloading successfully) --
; EnVar is a separate, third-party plugin
; (https://nsis.sourceforge.io/EnVar_plug-in, github.com/GsNSIS/EnVar) that
; nothing in this pipeline fetches or installs; (2) reproduced locally: a
; stock `apt install nsis` (v3.09, the same "just NSIS, nothing extra"
; baseline CI effectively gets) has no EnVar.dll in any of its Plugins/*
; directories, and compiling a minimal script that calls EnVar::SetHKCU
; against it fails with the exact same "Plugin not found" error.
;
; Fixed by dropping the EnVar dependency entirely and editing
; HKCU\Environment\Path with NSIS's own built-in instructions
; (ReadRegStr/WriteRegExpandStr/StrCpy/StrLen/IntOp, all present in every
; NSIS install, no plugin needed) plus the standard public-domain NSIS-wiki
; StrStr helper for substring search. Verified end-to-end under Wine (NSIS
; itself can only compile the script; only actually *running* the compiled
; installer/uninstaller proves the registry edits are correct), covering
; every case that matters for a real install/upgrade/uninstall cycle:
;   install, empty PATH                 -> Path = "$INSTDIR"
;   install again (same PATH)           -> unchanged, no duplicate
;   install, existing unrelated PATH    -> "<existing>;$INSTDIR"
;   install again                       -> unchanged, no duplicate
;   uninstall, entry in the middle      -> entry removed, neighbors intact
;   uninstall, entry at the end         -> entry removed, no trailing ";"
;   uninstall, entry at the start       -> entry removed, no leading ";"
;   uninstall, entry is the only value  -> Path becomes ""
;   uninstall, entry not present        -> unchanged (no-op)
;   uninstall, entry appears twice      -> both copies removed (defensive)
; An earlier draft of the StrStr helper had a real bug (measured the
; haystack's length instead of the needle's for the comparison window,
; `StrLen $R3 $R2` instead of `StrLen $R3 $R1`), which silently made every
; match fail (find nothing, so "install" would still work by falling
; through to append, but "uninstall" would never remove the entry) -- caught
; only by actually running the compiled .exe under Wine and inspecting the
; registry after each step, not by reading the script or by makensis
; compiling it cleanly (it compiles fine either way).
;
; HKCU, not HKLM, deliberately: (1) doesn't require a second elevation
; consideration beyond what installing to Program Files already needs, (2) a
; real, still-open NSIS bug (https://sourceforge.net/p/nsis/bugs/1247/)
; means editing HKLM's PATH can corrupt it on some Windows configurations if
; done carelessly -- HKCU doesn't carry that risk, and a per-user PATH entry
; is the right scope for a CLI tool anyway (no reason to touch every user's
; PATH on a shared machine for one user's install).
;
!include "LogicLib.nsh"
!ifndef HWND_BROADCAST
!define HWND_BROADCAST 0xFFFF
!endif
!ifndef WM_SETTINGCHANGE
!define WM_SETTINGCHANGE 0x001A
!endif

; Defined twice under NSIS's own convention -- once as a plain installer-
; section function (used from NSIS_HOOK_POSTINSTALL) and once with the
; mandatory "un." prefix (used from NSIS_HOOK_POSTUNINSTALL): NSIS keeps the
; install and uninstall code in genuinely separate binaries/scopes, so
; `Call StrStr` from an uninstall section fails to compile ("Call must be
; used with function names starting with 'un.' in the uninstall section")
; even though the function body is identical either way. !macro/!macroend
; here is purely a compile-time text-substitution trick to avoid keeping
; two hand-maintained copies of the same logic in sync.
!macro StrStrImpl un
Function ${un}StrStr
  Exch $R1 ; needle
  Exch     ; now $R1=needle is buried, haystack is on top
  Exch $R2 ; haystack
  Push $R3
  Push $R4
  Push $R5
  StrLen $R3 $R1
  StrCpy $R4 0
  loop:
    StrCpy $R5 $R2 $R3 $R4
    StrCmp $R5 $R1 done
    StrCmp $R5 "" done
    IntOp $R4 $R4 + 1
    Goto loop
  done:
  StrCpy $R1 $R2 "" $R4
  Pop $R5
  Pop $R4
  Pop $R3
  Pop $R2
  Exch $R1
FunctionEnd
!macroend
!insertmacro StrStrImpl ""
!insertmacro StrStrImpl "un."

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
  ReadRegStr $0 HKCU "Environment" "Path"
  ${If} $0 == ""
    StrCpy $0 "$INSTDIR"
  ${Else}
    ; skip if $INSTDIR is already present, so repeat installs/upgrades
    ; don't grow the PATH by one duplicate entry every time
    Push "$0;"
    Push "$INSTDIR;"
    Call StrStr
    Pop $1
    ${If} $1 == ""
      StrCpy $0 "$0;$INSTDIR"
    ${EndIf}
  ${EndIf}
  WriteRegExpandStr HKCU "Environment" "Path" $0
  ; Broadcast the PATH change so new shells/processes pick it up without a
  ; logoff/logon, matching what EnVar did internally.
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
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

  ; Undo the PATH addition above -- same HKCU scope, no plugin (see the
  ; long comment above NSIS_HOOK_POSTINSTALL for why EnVar was dropped).
  ; Sentinel-wrap with a leading/trailing ";" so $INSTDIR looks like
  ; ";$INSTDIR;" whether it was first, last, in the middle, or (twice, if
  ; some future rebuild of this PATH entry manages to double up) present
  ; more than once -- the removal loop below strips every occurrence
  ; uniformly instead of needing separate first/middle/last-entry cases.
  ReadRegStr $0 HKCU "Environment" "Path"
  StrCpy $0 ";$0;"
  removepath_loop:
    Push $0
    Push ";$INSTDIR;"
    Call un.StrStr
    Pop $1
    StrCmp $1 "" removepath_done
    StrLen $2 $0
    StrLen $3 $1
    IntOp $4 $2 - $3
    StrCpy $5 $0 $4
    StrLen $6 ";$INSTDIR"
    StrCpy $7 $1 "" $6
    StrCpy $0 "$5$7"
    Goto removepath_loop
  removepath_done:
  ; strip the sentinel semicolons back off
  StrCpy $0 $0 -1
  StrCpy $0 $0 "" 1
  WriteRegExpandStr HKCU "Environment" "Path" $0
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=5000
!macroend

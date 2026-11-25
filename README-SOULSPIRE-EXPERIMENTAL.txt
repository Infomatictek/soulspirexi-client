===============================================================================
SoulspireXI Experimental Client (based on VanaCore)
CLIENT-MODERN-001 — Phase 1 Minimal Runtime Verification Package
===============================================================================

STATUS: EXPERIMENTAL — NOT RUNTIME-CERTIFIED
  This package is a reproducible Windows x86 MSVC Release CI build derived from
  CatsAndBoats/VanaCore (pinned commit 04545ec52120c2df527ac3c0c4ab32392cfc10c5).
  Classic Ashita v4 remains the supported Stable default until live Windows
  runtime certification passes.

MINIMAL RUNTIME PROFILE (INTENTIONALLY SCOPED):
  - Connects to canonical SoulspireXI main server: play.soulspirexi.com
    (ports 54231 TLS auth / 54230 lobby data / 54001 lobby view)
  - Soulspire Native (soulspire_native.dll), Soulspire Core, custom weapon
    presentation, and experimental menu hooks are DISABLED by default.
  - Plaintext [server] password= in vanacore.ini and CLI --password / --pass
    arguments are DISABLED for credential safety.
  - Retail FFXI files are never modified; Pivot overlays read from pivot\soulspirexi\.

-------------------------------------------------------------------------------
1. INSTALLATION (SIDE-BY-SIDE — SAFE ALONGSIDE ASHITA v4)
-------------------------------------------------------------------------------
1. Extract this ZIP into a dedicated folder, e.g.:
     C:\SoulspireXI-Experimental\VanaCore\
2. Verify package integrity (optional PowerShell check):
     Get-FileHash .\VanaCore.exe -Algorithm SHA256
     (Compare against SHA256SUMS.txt and build-provenance.json.)
3. Copy vanacore.soulspire-minimal.ini to vanacore.ini in the same folder.
4. Edit vanacore.ini and uncomment/set your existing FFXI folders:
     [client]
     pol=C:\Program Files (x86)\PlayOnline\SquareEnix\PlayOnlineViewer
     ffxi=C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI
   (Optional: set [server] user=YourAccountName — NEVER put password= in the INI.)

-------------------------------------------------------------------------------
2. SECURE LAUNCH INSTRUCTIONS
-------------------------------------------------------------------------------
Option A — Interactive hidden password prompt (recommended for manual testing):
  Open PowerShell or Command Prompt in the extracted folder and run:
    .\VanaCore.exe --server play.soulspirexi.com --user YourAccountName
  Type your password at the hidden prompt (no echo) and press Enter.

Option B — Piped stdin (--password-stdin, for launcher/script integration):
  In PowerShell:
    $sec = Read-Host "SoulspireXI Password" -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
    try {
      [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr) |
        .\VanaCore.exe --server play.soulspirexi.com --user YourAccountName --password-stdin
    } finally {
      [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }

-------------------------------------------------------------------------------
3. MINIMAL RUNTIME VERIFICATION CHECKLIST (CLIENT-MODERN-001)
-------------------------------------------------------------------------------
Record PASS / FAIL / NOTES for each gate:
  [ ] 1. Windows launch: VanaCore.exe injects VanaCore.Core.dll cleanly.
  [ ] 2. Authentication: TLS login to play.soulspirexi.com:54231 succeeds.
  [ ] 3. Character select: Lobby connects and displays SoulspireXI characters.
  [ ] 4. Zone entry & movement: Character loads into world and moves normally.
  [ ] 5. Combat, inventory & equipment: Engage target, open inventory/equip menus.
  [ ] 6. Input: Keyboard, mouse, and controller (if attached) respond properly.
  [ ] 7. Zone transition, logout & reconnect: Change zone, /logout, re-enter cleanly.
  [ ] 8. DX12 stability: Check FPS, window resize/alt-tab, and zero D3D12 device loss
         (note: if DX12 fails on your GPU/driver, retry with renderer=d3d11 in vanacore.ini).

-------------------------------------------------------------------------------
4. REQUIRED DIAGNOSTIC LOGS
-------------------------------------------------------------------------------
After testing (or immediately if any error/crash occurs), preserve and attach:
  - logs\vanacore.log
  - build-provenance.json
  - Exact GPU model, Windows build (winver), and whether renderer=d3d12 or d3d11 was used.

Do NOT share your password. VanaCore never writes passwords to logs\vanacore.log.

-------------------------------------------------------------------------------
5. LICENSING & ATTRIBUTION
-------------------------------------------------------------------------------
Based on VanaCore (Copyright (C) 2026 Vekien), licensed under GNU GPL-3.0-or-later
with GNU GPL Section 7 additional permissions and terms. This SoulspireXI derivative
replaces all proprietary Vekien logo artwork per GPL Section 7 Term 4 and is not
endorsed by or affiliated with Vekien, CatsEyeXI, or Square Enix.
See LICENSE.txt, LICENSING.md, THIRD_PARTY_NOTICES.md, and licenses\ for full terms.

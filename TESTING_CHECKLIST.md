# SoulspireXI Client (VanaCore) — Phase 1 Minimal Runtime Test Checklist

## Prerequisites
- Existing Windows FFXI + PlayOnline installation (never mutated).
- Valid SoulspireXI account on canonical `play.soulspirexi.com` (do NOT create a separate game server).
- Download `SoulspireClient-<version>-<sha>-win32-testing.zip` and `.sha256` from GitHub Actions (`Infomatictek/soulspirexi-client`).

## Install (Isolated Side-by-Side Directory)
1. Verify ZIP SHA-256 against the `.sha256` file:
   `Get-FileHash .\SoulspireClient-*.zip -Algorithm SHA256`
2. Extract to an isolated folder beside your existing Ashita install, e.g. `C:\SoulspireXI\ModernClient\` (never overwrite `Ashita\` or `Game\`).
3. Copy `vanacore.minimal.ini` to `vanacore.ini` and `scripts\boot.minimal.txt` to `scripts\boot.txt`.
4. Set `[client] pol=` and `[client] ffxi=` in `vanacore.ini` to your base `PlayOnlineViewer` and `FINAL FANTASY XI` paths (or launch via SoulspireXI Launcher Modern [Experimental] mode).

## Secure Launch (No Plaintext Password Files or CLI Password Exposure)
Option A (Recommended): Launch via **SoulspireXI Launcher** (Testing Channel -> Client Engine: **Modern [Experimental]** -> Phase 1 Safe Mode), which passes credentials via `--password-stdin`.

Option B (Manual PowerShell prompt, never echoed or saved to disk):
```powershell
$pw = Read-Host -AsSecureString "SoulspireXI Password"
$plain = [Runtime.InteropServices.Marshal]::PtrToStringUni([Runtime.InteropServices.Marshal]::SecureStringToBSTR($pw))
$plain | .\VanaCore.exe --ini vanacore.minimal.ini --server play.soulspirexi.com --user YOUR_USERNAME --password-stdin
$plain = $null
```

## Phase 1 Minimal Verification Checklist (Baseline Login Only)
- [ ] TLS auth succeeds against `play.soulspirexi.com:54231` (`Logged in.`).
- [ ] `polcore.dll` boots to Character Select without PlayOnline crash.
- [ ] Character enters game world (`0x00A`), movement & camera work, chat sends/receives.
- [ ] Zone transition succeeds cleanly without hang or disconnect.
- [ ] Logout & clean exit work with zero crash dump.
- [ ] `logs\vanacore.log` shows `FFXiMain.dll` `header 6AE818B4, image C17000` and lists any missed signature patterns.

## Required Logs & Screenshots to Report Back
1. `ModernClient\logs\vanacore.log` (redact any personal Windows paths if desired; never contains passwords).
2. Screenshot of Character Select and in-world viewport (confirming clean rendering and SoulspireXI window title).
3. Any crash dump under `ModernClient\logs\` if an error occurred.

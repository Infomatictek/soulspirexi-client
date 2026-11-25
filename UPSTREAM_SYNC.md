# Upstream Synchronization Strategy (`CatsAndBoats/VanaCore`)

This repository (`Infomatictek/soulspirexi-client`) tracks the SoulspireXI modern client migration (`CLIENT-MODERN-001`, governed by `DEC-025` and `DEC-026` in `Infomatictek/soulspirexi`).

It is derived from [CatsAndBoats/VanaCore](https://github.com/CatsAndBoats/VanaCore) with full upstream Git history and licensing preserved.

---

## 1. Pinned Upstream Baseline

| Field | Value |
|---|---|
| Upstream repository | `https://github.com/CatsAndBoats/VanaCore.git` |
| Upstream remote name | `upstream` |
| Pinned audited commit | `04545ec52120c2df527ac3c0c4ab32392cfc10c5` |
| Reference Git tag | `upstream-04545ec5` |
| Upstream license | `GPL-3.0-or-later` with GNU GPL Section 7 additional permissions (NVIDIA DLSS SDK, AMD FSR SDK, game/plugins/addons) and Section 7 Term 4 (name & logo trademark / proprietary artwork restriction) |

### Configuring Remotes Locally

```bash
git remote add upstream https://github.com/CatsAndBoats/VanaCore.git
git fetch upstream
```

---

## 2. Branching & Isolation Model

- **`main`**: Tracks the owner-approved SoulspireXI client baseline. Never force-push `main`.
- **Feature branches (`feat/*`, `fix/*`, `sync/*`)**: All SoulspireXI customizations, CI changes, and upstream merges are developed and verified in CI on dedicated branches before owner-authorized promotion.
- **Minimal divergence principle**: Keep SoulspireXI-specific deltas cleanly separated and well-scoped so upstream bug fixes, DX12 renderer improvements, and FFXI signature updates can be merged cleanly without conflicts.

---

## 3. Upstream Synchronization Procedure

When evaluating a newer upstream `CatsAndBoats/VanaCore` commit:

1. **Fetch & Inspect**
   ```bash
   git fetch upstream --prune
   git log --oneline upstream-04545ec5..upstream/main
   ```
2. **Audit Before Merging**
   - Review changes to `src/core/sigs.cpp`, `src/core/input.cpp`, `src/launcher/main.cpp`, `src/core/pivot.cpp`, `src/core/discord.cpp`, `src/gfx/`, and `LICENSING.md` / `THIRD_PARTY_NOTICES.md`.
   - Verify no CatsEyeXI-specific server defaults, proprietary Vekien logo artwork, plaintext credential handling, or CatsEyeXI-only UI assumptions re-enter SoulspireXI builds.
   - Verify no unguarded fixed RVAs were introduced that could crash against SoulspireXI's `FFXiMain.dll`.
3. **Sync on a Dedicated Branch**
   ```bash
   git checkout -b sync/upstream-<shortsha> origin/main
   git merge --no-ff <target-upstream-sha>
   ```
4. **Verify via Windows x86 MSVC CI**
   - Push the sync branch and confirm `.github/workflows/build-windows-x86.yml` completes green (`Release|x86` + `VanaCore.Dlss` `Release|x64`, `VanaCore.SettingsTest` passing, SHA-256 checksums, dependency inventory, build provenance, and licensing compliance checks passing).
5. **Runtime Verification Gate (`DEC-002`)**
   - Compilation passing in CI never certifies runtime compatibility. Run the Windows runtime smoke checklist against `play.soulspirexi.com` and attach `logs\vanacore.log` evidence before owner-authorized promotion.

---

## 4. License & Redistribution Compliance

Every build artifact generated from this repository MUST:

1. Preserve `LICENSE` (`LICENSE.txt`), `LICENSING.md`, `THIRD_PARTY_NOTICES.md`, and `licenses/` (`LICENSE-NVIDIA-DLSS.txt`, `LICENSE-AMD-FSR-SDK.md`, and Ashita SDK GPL/LGPL texts).
2. Comply with GPL Section 7 Term 4 in any modified SoulspireXI distribution:
   - Never present a modified build as upstream VanaCore or as made/endorsed by Vekien (state clearly that it is based on VanaCore).
   - Remove and replace all proprietary all-rights-reserved Vekien logo images (`assets/logo*.png`, `addons/vanacore/vana/logo.png`, `src/launcher/VanaCore.ico`).
3. Verify third-party binary SDKs strictly against pinned SHA-256 hashes (`third_party/dlss/fetch-dlss.ps1`, `third_party/fsr/fetch-fsr.ps1`) and never bundle restricted binaries such as `nvngx_dlssnr.dll`.

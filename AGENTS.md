# SoulspireXI Client (`Infomatictek/soulspirexi-client`) — AI Agent Guide

## Authoritative Context Recovery

Before making changes, recover full cross-repository project context from the Project Control Center in **`Infomatictek/soulspirexi`**:

- Primary recovery doc: `docs/project/AI_CONTEXT_RECOVERY.md`
- Active tasks & WIP cap (max 3): `docs/project/ACTIVE_TASKS.md`
- Open blockers: `docs/project/BLOCKERS.md`
- Architecture & governance decisions (`DEC-001`–`DEC-030`, including `DEC-030` GitHub synchronization & task completion contract): `docs/project/DECISIONS.md` and `Infomatictek/soulspirexi` `AGENTS.md` Section 5
- System & deployment registry: `docs/project/SYSTEM_REGISTRY.md`
- Native client migration architecture: `docs/architecture/CLIENT_MODERN_001_VANACORE_NATIVE_ARCHITECTURE.md`

## Repository Role & Canonical Branch

- **Purpose:** SoulspireXI's native Windows FFXI client fork based on upstream [`CatsAndBoats/VanaCore`](https://github.com/CatsAndBoats/VanaCore) (GPL-3.0-or-later with Section 7 Term 4).
- **Canonical SoulspireXI Development Branch:** `feat/vanacore-native-soulspire` (tagged as `v0.9.9-soulspire-native.<N>`), **NOT** upstream `main`.
- **Upstream Baseline (`main`):** Intentionally tracks upstream `CatsAndBoats/VanaCore` commit `04545ec52120c2df527ac3c0c4ab32392cfc10c5` (do not commit Soulspire changes to `main` or blindly rebase without reviewing `UPSTREAM_SYNC.md` and `docs/retail-signatures.md`).
- **New Worktrees & Branches:** Always create new SoulspireXI client development worktrees and feature branches from `origin/feat/vanacore-native-soulspire` (e.g., `git worktree add -b feat/<name> <path> origin/feat/vanacore-native-soulspire`), never from upstream `main`.
- **Sync & Completion Contract (`DEC-030`):** All tasks must satisfy the 9-point GitHub synchronization and completion contract in `Infomatictek/soulspirexi` `AGENTS.md` Section 5.

## Non-Negotiable Rules

1. **Licensing & Branding Hygiene (GPL-3.0-or-later + Section 7 Term 4):**
   - Run `python3 tools/verify_soulspire_hygiene.py` before every push.
   - Never restore upstream CatsEyeXI branding, proprietary Vekien artwork, `server.catseyexi.com`, CatsEyeXI Discord IDs, or `dlss/nvngx_dlssnr.dll`.
2. **Credential Security:**
   - Never accept plaintext passwords on the command line (`--password` / `--pass` must remain refused); authenticate via `--password-stdin` and scrub buffers with `SecureZeroMemory`.
   - Never log credentials, session hashes, or tokens.
3. **Retail Binary & Registry Compatibility:**
   - Retail `polcore.dll` (`3C07C1A3`) decrypts `FINAL FANTASY XI\patch.ver` using the install-specific key in real `HKLM\SOFTWARE\WOW6432Node\PlayOnline*\Interface\0001`.
   - `src/core/registry.cpp` and `src/core/registry_policy.hpp` MUST keep `Interface` read-only passthrough to real HKLM (never mutating the user's real Windows registry) and fall back to HKCU stand-in only if real HKLM `Interface` is absent.
   - `src/core/loader.cpp` and `src/core/loader_startup.hpp` MUST classify any pre-`Direct3DCreate8` `GameStart` return (including `S_OK` / `0x00000000`) as a startup failure (`kExitGameStartFailed`).
4. **Testing & Build Verification:**
   - GitHub Actions workflow `.github/workflows/build-windows-x86.yml` builds `Release|x86` (`VanaCore.sln`), executes `VanaCore.SettingsTest.exe`, compiles `VanaCore.GfxTest`, runs `tools/verify_soulspire_hygiene.py`, and publishes `v*` tags as prereleases with `ARTIFACT_SHA256SUMS.txt` and `build-provenance.json`.
   - Never mark a build `clientVerified: true` / `runtime_certified: true` from CI or Wine alone; live native Windows verification against `play.soulspirexi.com` is required (`DEC-002` / `BLK-005`).

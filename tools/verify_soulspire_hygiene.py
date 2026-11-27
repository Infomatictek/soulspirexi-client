#!/usr/bin/env python3
"""Verify SoulspireXI Client GPL-3.0-or-later, Term 4 branding, security, and compatibility invariants."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

LEGACY_NAMESPACE_RE = re.compile(r"\b(overlords?xi|overlords|overlords_[a-z0-9_]+|ox_combat_session)\b", re.IGNORECASE)

ROOT = Path(__file__).resolve().parent.parent

# Replacement SoulspireXI branding assets that must exist and never be huge upstream Vekien originals.
REPLACED_BRANDING_ASSETS = {
    "assets/logo.png",
    "assets/logo_cutout.png",
    "assets/logo_square.png",
    "addons/vanacore/vana/logo.png",
    "src/launcher/VanaCore.ico",
}

# Upstream proprietary Vekien artwork that must be deleted (not distributed).
FORBIDDEN_UPSTREAM_ARTWORK = [
    "assets/banner.png",
    "assets/download.png",
    "assets/logo_text.png",
    "assets/logo_text_transparent.png",
    "addons/vanacore/vana/banner.png",
]

FORBIDDEN_FILES = [
    ".github/workflows/release.yaml",
    ".github/workflows/promote.yaml",
    ".github/scripts/push_build.py",
]

BASE_FORBIDDEN_STRINGS = [
    ("src/launcher/settings.hpp", "server.catseyexi.com"),
    ("src/launcher/main.cpp", "CatsEyeXI"),
    ("src/core/pivot.cpp", 'L"catseyexi"'),
    ("src/core/discord.cpp", "1560327334966337547"),
    ("src/core/discord.cpp", '"CatsEyeXI"'),
    ("src/core/gmicons_staff.h", "server.catseyexi.com"),
    ("src/core/gmicons_staff.h", "kCatsEyeXI"),
    ("addons/vanacore/vana/data.lua", "'catseyexi'"),
    ("addons/vanacore/vana/data.lua", "CatsEyeXI"),
    ("addons/vanacore/vana/data.lua", "cexidats"),
    ("addons/vanacore/vana/data.lua", "cexislots"),
]

COMPAT_FORBIDDEN_STRINGS = [
    ("src/core/plugins.cpp", "required by CatsEyeXI"),
    ("src/core/addons.cpp", '"cexidats"'),
    ("src/core/addons.cpp", '"cexislots"'),
    ("src/core/addons.cpp", '"customcolors"'),
    ("src/core/addons.cpp", "required by CatsEyeXI"),
]

BASE_REQUIRED_MARKERS = [
    ("src/launcher/settings.hpp", 'L"play.soulspirexi.com"'),
    ("src/launcher/main.cpp", "--password-stdin"),
    ("src/launcher/main.cpp", "read_password_stdin"),
    ("src/launcher/main.cpp", "SecureZeroMemory"),
    ("src/core/pivot.cpp", 'L"soulspirexi"'),
    ("LICENSING.md", "https://github.com/Infomatictek/soulspirexi-client"),
    ("LICENSING.md", "04545ec52120c2df527ac3c0c4ab32392cfc10c5"),
]

COMPAT_REQUIRED_MARKERS = [
    ("src/core/lua_api.hpp", "ashita_interface = 4.30"),
    ("src/core/lua_api.cpp", 'L"4.30"'),
    ("src/core/addons.hpp", "on_dinput_state"),
    ("src/core/addons.cpp", '"dinput_state"'),
    ("src/core/gamepad.cpp", "addons::on_dinput_state"),
    ("src/core/lua/compat/ashita.lua", "VC_XINPUT_STATE*"),
    ("src/core/lua/compat/ashita.lua", "VC_DIJOYSTATE*"),
    ("src/core/singlemenu.cpp", "soulspire_native.dll"),
    ("src/core/sigs.cpp", "0x6AE818B4"),
]

REQUIRED_FILES = [
    "LICENSE",
    "LICENSING.md",
    "THIRD_PARTY_NOTICES.md",
    "vanacore.soulspire-minimal.ini",
    "README-SOULSPIRE-EXPERIMENTAL.txt",
    "scripts/boot.txt",
    ".github/workflows/build-windows-x86.yml",
]


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify SoulspireXI Client GPL/branding/security hygiene.")
    parser.add_argument("--mode", choices=("auto", "baseline", "compat"), default="auto")
    parser.add_argument("--package-dir", type=Path, default=None, help="Optional built package dir to verify.")
    args = parser.parse_args()

    mode = args.mode
    if mode == "auto":
        sigs_text = (ROOT / "src/core/sigs.cpp").read_text(encoding="utf-8", errors="replace") if (ROOT / "src/core/sigs.cpp").is_file() else ""
        mode = "compat" if "0x6AE818B4" in sigs_text else "baseline"

    errors: list[str] = []

    for rel in REQUIRED_FILES:
        if not (ROOT / rel).is_file():
            errors.append(f"Missing required file: {rel}")

    for rel in FORBIDDEN_FILES:
        if (ROOT / rel).exists():
            errors.append(f"Upstream CatsEyeXI/R2/Azure file must be removed: {rel}")

    for rel in FORBIDDEN_UPSTREAM_ARTWORK:
        if (ROOT / rel).exists():
            errors.append(f"GPL Section 7 Term 4 violation: proprietary artwork still present at {rel}")

    forbidden_strings = list(BASE_FORBIDDEN_STRINGS)
    required_markers = list(BASE_REQUIRED_MARKERS)
    if mode == "compat":
        forbidden_strings.extend(COMPAT_FORBIDDEN_STRINGS)
        required_markers.extend(COMPAT_REQUIRED_MARKERS)

    for rel, forbidden in forbidden_strings:
        p = ROOT / rel
        if not p.is_file():
            errors.append(f"Expected source file missing: {rel}")
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        if forbidden in text:
            errors.append(f"Forbidden CatsEyeXI hardcode {forbidden!r} found in {rel}")

    for rel, required in required_markers:
        p = ROOT / rel
        if not p.is_file():
            errors.append(f"Expected source file missing: {rel}")
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        if required not in text:
            errors.append(f"Missing required SoulspireXI marker {required!r} in {rel}")

    for rel in sorted(REPLACED_BRANDING_ASSETS):
        p = ROOT / rel
        if not p.is_file() or p.stat().st_size == 0:
            errors.append(f"Replacement SoulspireXI branding asset missing or empty: {rel}")
        elif rel.endswith(".png") and p.stat().st_size > 500_000:
            errors.append(f"Suspiciously large PNG ({p.stat().st_size}B) at {rel} — verify Vekien logo was replaced")

    # BRAND-001 / DEC-033 permanent SoulspireXI namespace hygiene across Soulspire-owned client sources
    for scan_dir in ("src/core/soulspire", "src/launcher"):
        base = ROOT / scan_dir
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file() or p.suffix.lower() not in (".h", ".hpp", ".c", ".cpp", ".ini", ".txt"):
                continue
            txt = p.read_text(encoding="utf-8", errors="replace")
            hits = sorted({m.group(0) for m in LEGACY_NAMESPACE_RE.finditer(txt)})
            if hits:
                errors.append(
                    f"BRAND-001 legacy namespace identifier(s) {', '.join(hits)} found in {p.relative_to(ROOT).as_posix()}"
                )

    if args.package_dir is not None:
        pkg = args.package_dir
        for req in ("VanaCore.exe", "VanaCore.Core.dll", "LICENSE.txt", "LICENSING.md", "THIRD_PARTY_NOTICES.md",
                    "vanacore.soulspire-minimal.ini", "README-SOULSPIRE-EXPERIMENTAL.txt", "scripts/boot.txt"):
            if not (pkg / req).is_file():
                errors.append(f"Built package missing required file: {req}")
        if (pkg / "dlss" / "nvngx_dlssnr.dll").exists():
            errors.append("Built package must NEVER contain unreleased nvngx_dlssnr.dll")

    if errors:
        for err in errors:
            print(f"[ERROR] {err}", file=sys.stderr)
        return 1

    print(f"[OK] SoulspireXI Client hygiene checks passed (mode={mode}).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

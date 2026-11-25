#!/usr/bin/env python3
"""Verify SoulspireXI Client GPL-3.0-or-later, Term 4 branding, security, and compatibility invariants."""
from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# SHA-256 digests of upstream proprietary Vekien logo images (must never remain in SoulspireXI builds).
FORBIDDEN_UPSTREAM_LOGO_SHA256 = {
    "assets/logo.png",
    "assets/logo_cutout.png",
    "assets/logo_square.png",
    "assets/logo_text.png",
    "assets/logo_text_transparent.png",
    "addons/vanacore/vana/logo.png",
    "src/launcher/VanaCore.ico",
}

FORBIDDEN_RUNTIME_STRINGS = [
    ("src/launcher/settings.hpp", "server.catseyexi.com"),
    ("src/launcher/main.cpp", "CatsEyeXI"),
    ("src/core/pivot.cpp", 'L"catseyexi"'),
    ("src/core/discord.cpp", "1560327334966337547"),
    ("src/core/discord.cpp", '"CatsEyeXI"'),
    ("src/core/gmicons_staff.h", "server.catseyexi.com"),
    ("src/core/gmicons_staff.h", "kCatsEyeXI"),
    ("src/core/plugins.cpp", "required by CatsEyeXI"),
    ("src/core/addons.cpp", '"cexidats"'),
    ("src/core/addons.cpp", '"cexislots"'),
    ("src/core/addons.cpp", '"customcolors"'),
    ("src/core/addons.cpp", "required by CatsEyeXI"),
    ("addons/vanacore/vana/data.lua", "'catseyexi'"),
    ("addons/vanacore/vana/data.lua", "CatsEyeXI's own"),
]

REQUIRED_CODE_MARKERS = [
    ("src/launcher/settings.hpp", 'L"play.soulspirexi.com"'),
    ("src/launcher/main.cpp", "--password-stdin"),
    ("src/launcher/main.cpp", "read_password_from_stdin"),
    ("src/launcher/main.cpp", "SecureZeroMemory"),
    ("src/core/pivot.cpp", 'L"soulspirexi"'),
    ("src/core/lua_api.hpp", "ashita_interface = 4.30"),
    ("src/core/lua_api.cpp", 'L"4.30"'),
    ("src/core/addons.hpp", "on_dinput_state"),
    ("src/core/addons.cpp", '"dinput_state"'),
    ("src/core/gamepad.cpp", "addons::on_dinput_state"),
    ("src/core/lua/compat/ashita.lua", "VC_XINPUT_STATE*"),
    ("src/core/lua/compat/ashita.lua", "VC_DIJOYSTATE*"),
    ("src/core/singlemenu.cpp", "soulspire_native.dll"),
    ("src/core/sigs.cpp", "0x6AE818B4"),
    ("LICENSING.md", "https://github.com/Infomatictek/soulspirexi-client"),
    ("LICENSING.md", "04545ec52120c2df527ac3c0c4ab32392cfc10c5"),
]

REQUIRED_FILES = [
    "LICENSE",
    "LICENSING.md",
    "THIRD_PARTY_NOTICES.md",
    "vanacore.minimal.ini",
    "scripts/boot.minimal.txt",
    "TESTING_CHECKLIST.md",
    ".github/workflows/build-client.yml",
]

FORBIDDEN_FILES = [
    ".github/workflows/release.yaml",
    ".github/workflows/promote.yaml",
    ".github/scripts/push_build.py",
]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify SoulspireXI Client GPL/branding/security hygiene.")
    parser.add_argument("--package-dir", type=Path, default=None, help="Optional built package dir to verify.")
    args = parser.parse_args()

    errors: list[str] = []

    for rel in REQUIRED_FILES:
        if not (ROOT / rel).is_file():
            errors.append(f"Missing required file: {rel}")

    for rel in FORBIDDEN_FILES:
        if (ROOT / rel).exists():
            errors.append(f"Upstream CatsEyeXI/R2/Azure file must be removed: {rel}")

    for rel, forbidden in FORBIDDEN_RUNTIME_STRINGS:
        p = ROOT / rel
        if not p.is_file():
            errors.append(f"Expected source file missing: {rel}")
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        if forbidden in text:
            errors.append(f"Forbidden CatsEyeXI hardcode {forbidden!r} found in {rel}")

    for rel, required in REQUIRED_CODE_MARKERS:
        p = ROOT / rel
        if not p.is_file():
            errors.append(f"Expected source file missing: {rel}")
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        if required not in text:
            errors.append(f"Missing required SoulspireXI marker {required!r} in {rel}")

    # Verify replacement logo assets exist and are non-empty.
    for rel in sorted(FORBIDDEN_UPSTREAM_LOGO_SHA256):
        p = ROOT / rel
        if not p.is_file() or p.stat().st_size == 0:
            errors.append(f"Replacement SoulspireXI branding asset missing or empty: {rel}")
        # Original Vekien logo.png was 1,251,763 bytes; ensure none of the huge proprietary originals remain.
        elif rel.endswith(".png") and p.stat().st_size > 500_000:
            errors.append(f"Suspiciously large PNG ({p.stat().st_size}B) at {rel} — verify Vekien logo was replaced")

    if args.package_dir is not None:
        pkg = args.package_dir
        for req in ("VanaCore.exe", "VanaCore.Core.dll", "LICENSE.txt", "LICENSING.md", "THIRD_PARTY_NOTICES.md",
                    "vanacore.minimal.ini", "scripts/boot.minimal.txt", "TESTING_CHECKLIST.md"):
            if not (pkg / req).is_file():
                errors.append(f"Built package missing required file: {req}")
        if (pkg / "dlss" / "nvngx_dlssnr.dll").exists():
            errors.append("Built package must NEVER contain unreleased nvngx_dlssnr.dll")

    if errors:
        for err in errors:
            print(f"[ERROR] {err}", file=sys.stderr)
        return 1

    print("[OK] SoulspireXI Client GPL-3.0-or-later, branding, security (--password-stdin), and BLK-013 hygiene checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Extract the default .conf from src/Config.h, write it with CRLF line endings,
and check its structure.

The plugin writes this exact text when MagickaPoolFix.conf is missing, so the file
produced here is byte-identical to the one a player gets on first launch (the
plugin turns every LF into CRLF as it writes, see WriteDefaultConfig).

Usage: gen_conf.py <path/to/Config.h> <output.conf>
Exits non-zero, after listing every failure, if a check fails.
"""
import os
import re
import sys

BANNER = "A D V A N C E D"

# Settings a regular player needs. Everything else belongs below the banner.
PLAYER_SECTIONS = {"General", "Magicka", "Log"}

# Shipped defaults: each must appear exactly once, with exactly this value.
DEFAULTS = {
    "bDryRun": "0",
    "iPreset": "2",
    "iEnginePolicy": "1",
    "iPolicyConfirmations": "1",
    "bCallVirtuals": "0",
    "iDumpKey": "0x79",
}

# Keys that must never be bound to a quick save / quick load key.
FORBIDDEN_KEYS = {"0x74": "F5", "0x77": "F8", "0x78": "F9"}

SETTING_RE = re.compile(r"^[A-Za-z_]\w*\s*=\s*[^\s;]+\s*(;.*)?$")
SECTION_RE = re.compile(r"^\[(\w+)\]\s*$")


def plugin_version(config_h: str) -> str:
    pch = os.path.join(os.path.dirname(config_h), "PCH.h")
    m = re.search(r'#define\s+MPF_PLUGIN_VERSION\s+"([^"]+)"', open(pch, encoding="utf-8").read())
    return m.group(1) if m else ""


def extract(config_h: str) -> str:
    src = open(config_h, encoding="utf-8").read()
    m = re.search(r'R"CONF\((.*?)\)CONF"', src, re.S)
    if not m:
        sys.exit(f"{config_h}: no R\"CONF(...)CONF\" literal found")
    return m.group(1)


def check(text: str, version: str) -> list[str]:
    errors = []
    lines = text.split("\n")

    try:
        text.encode("ascii")
    except UnicodeEncodeError as e:
        errors.append(f"non-ASCII character at offset {e.start}")

    banners = [i for i, l in enumerate(lines) if BANNER in l]
    if len(banners) != 1:
        errors.append(f"expected exactly one '{BANNER}' banner, found {len(banners)}")
    banner_at = banners[0] if banners else len(lines)

    # The conf must not carry the plugin version: it outlives the binary that wrote it.
    if version and version in text:
        errors.append(f"current plugin version {version} appears in the conf text")

    section = None
    seen = {}
    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue
        sm = SECTION_RE.match(line)
        if sm:
            section = sm.group(1)
            if n - 1 < banner_at and section not in PLAYER_SECTIONS:
                errors.append(f"line {n}: section [{section}] is above the banner")
            continue
        if not SETTING_RE.match(line):
            errors.append(f"line {n}: malformed setting: {line!r}")
            continue
        key, value = (p.strip() for p in line.split(";", 1)[0].split("=", 1))
        seen.setdefault(key, []).append(value)
        if n - 1 < banner_at and section not in PLAYER_SECTIONS:
            errors.append(f"line {n}: {key} in [{section}] is above the banner")
        if key.lower().endswith("key") and value.lower() in FORBIDDEN_KEYS:
            errors.append(f"line {n}: {key} bound to {FORBIDDEN_KEYS[value.lower()]}")

    for key, want in DEFAULTS.items():
        got = seen.get(key, [])
        if got != [want]:
            errors.append(f"{key}: expected exactly one line '= {want}', found {got}")

    return errors


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    text = extract(sys.argv[1])
    errors = check(text, plugin_version(sys.argv[1]))
    with open(sys.argv[2], "w", encoding="ascii", errors="replace", newline="\r\n") as f:
        f.write(text)
    if errors:
        for e in errors:
            print(f"conf check FAIL: {e}")
        sys.exit(1)
    print(f"conf check OK: {sys.argv[2]} ({text.count(chr(10))} lines, ASCII, CRLF)")


if __name__ == "__main__":
    main()

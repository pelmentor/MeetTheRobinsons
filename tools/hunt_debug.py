"""
hunt_debug.py - hunt for debug features and hidden launch flags in Wilbur_unpacked.exe.

Targets:
1. Command-line flags ("-foo", "/foo", "--foo")
2. Console / cvar names
3. Cheat codes / debug commands (GODMODE, NOCLIP, etc.)
4. Debug-draw / visualization toggles
5. Internal verbosity / log channel names
6. Developer keybinds and shortcuts

Usage:
    python tools/hunt_debug.py ida/Wilbur_unpacked.exe
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from collections import Counter, defaultdict


CHEAT_KEYWORDS = (
    "godmode", "noclip", "invincible", "infinite", "unlimited", "cheat",
    "ghost", "fly", "skip", "unlock", "give", "spawn", "kill", "teleport",
    "tp_", "warp", "tp ", "freecam", "flycam", "freelook", "thirdperson",
    "firstperson", "nofog", "wireframe", "fullbright", "showfps",
    "showbboxes", "showtriggers", "shownormals", "showpaths",
    "ai_disable", "physics_disable", "loglevel", "verbose",
)

DEBUG_KEYWORDS = (
    "debug", "dbg", "dev_", "_dev", "developer", "internal", "test_",
    "_test", "trace", "log_", "verbose", "diag", "profile",
)

CONSOLE_HINTS = (
    "console", "cmd", "command", "exec", "bind", "alias", "cvar",
    "set ", "get ", "enable", "disable", "toggle",
)


def scan_strings(data: bytes, min_len: int = 4) -> list[bytes]:
    pattern = re.compile(rb"[\x20-\x7E]{" + str(min_len).encode() + rb",}")
    return list(pattern.findall(data))


def find_flags(strings: list[str]) -> dict[str, list[str]]:
    """Find -flag, /flag, --flag patterns."""
    out: dict[str, list[str]] = defaultdict(list)
    for s in strings:
        # match standalone short flags like "-flag" or "-flag=...", "-flag value"
        for m in re.finditer(r'(?<![/\w])-(-?[a-zA-Z][a-zA-Z0-9_]{2,30})\b', s):
            flag = m.group(1)
            if not any(c.isupper() for c in flag) or flag.lower() == flag:
                # skip CamelCase/UPPERCASE single-word that look like format strings
                pass
            out[f"-{flag}"].append(s.strip())
    return out


def find_cvars_and_commands(strings: list[str]) -> list[tuple[str, str]]:
    """Look for command-name + description pairs typical of dev consoles."""
    out: list[tuple[str, str]] = []
    for s in strings:
        # patterns like "command_name (description)" or "command\tdescription"
        m = re.match(r'^([a-z][a-z_0-9]{3,30})\s+(.{8,})', s)
        if m:
            cmd, desc = m.group(1), m.group(2)
            if any(kw in s.lower() for kw in DEBUG_KEYWORDS + CHEAT_KEYWORDS + CONSOLE_HINTS):
                out.append((cmd, desc))
    return out


def find_keyword_strings(strings: list[str], keywords: tuple[str, ...]) -> list[str]:
    out = []
    for s in strings:
        sl = s.lower()
        if any(kw in sl for kw in keywords):
            out.append(s.strip())
    return out


def find_camel_case_idents(strings: list[str]) -> list[str]:
    """Find CamelCase / snake_case identifiers that look like internal names."""
    out = []
    for s in strings:
        if re.match(r'^[a-zA-Z][a-zA-Z0-9_]{6,40}$', s):
            # Single identifier-shaped string
            if (re.search(r'[a-z][A-Z]', s) or '_' in s):  # CamelCase or snake_case
                out.append(s)
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python tools/hunt_debug.py <pe-file>", file=sys.stderr)
        return 2

    p = Path(sys.argv[1])
    data = p.read_bytes()
    print(f"scanning: {p}  ({len(data):,} bytes)\n")

    raw = scan_strings(data)
    strings = [s.decode("ascii", errors="replace") for s in raw]
    print(f"{len(strings):,} ASCII strings (>=4 chars)\n")

    # 1. Command-line flags
    flags = find_flags(strings)
    print(f"=" * 70)
    print(f"COMMAND-LINE FLAGS  ({len(flags)} unique)")
    print(f"=" * 70)
    # Filter to interesting-looking ones (length, lowercase preferred)
    interesting_flags = sorted([f for f in flags if 3 <= len(f) <= 32])
    for flag in interesting_flags:
        contexts = list(set(flags[flag]))[:2]
        print(f"  {flag:<32} | {contexts[0][:80] if contexts else ''}")

    # 2. Cheat-keyword strings
    print(f"\n{'=' * 70}")
    print(f"CHEAT / GAMEPLAY-MOD KEYWORDS")
    print(f"{'=' * 70}")
    cheats = sorted(set(find_keyword_strings(strings, CHEAT_KEYWORDS)))
    for s in cheats[:60]:
        print(f"  {s[:100]}")
    if len(cheats) > 60:
        print(f"  ... and {len(cheats) - 60} more")

    # 3. Debug-keyword strings (filtered to short, identifier-like)
    print(f"\n{'=' * 70}")
    print(f"DEBUG-RELATED IDENTIFIERS / SHORT STRINGS")
    print(f"{'=' * 70}")
    debug_strs = set(find_keyword_strings(strings, DEBUG_KEYWORDS))
    short_debug = sorted([s for s in debug_strs if len(s) < 60])
    for s in short_debug[:80]:
        print(f"  {s}")
    if len(short_debug) > 80:
        print(f"  ... and {len(short_debug) - 80} more")

    # 4. Console / command-style strings
    print(f"\n{'=' * 70}")
    print(f"CONSOLE / COMMAND HINTS")
    print(f"{'=' * 70}")
    console_strs = sorted(set(find_keyword_strings(strings, CONSOLE_HINTS)))
    short_console = [s for s in console_strs if len(s) < 80]
    for s in short_console[:40]:
        print(f"  {s}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

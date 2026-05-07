"""
hunt_debug_focus.py - drill into specific interesting strings.

Targeted searches based on hunt_debug.py first pass.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


def scan_strings(data: bytes, min_len: int = 4) -> list[str]:
    pattern = re.compile(rb"[\x20-\x7E]{" + str(min_len).encode() + rb",}")
    return [m.decode("ascii", errors="replace") for m in pattern.findall(data)]


def grep(strings: list[str], regex: str, *, ic: bool = True) -> list[str]:
    flags = re.IGNORECASE if ic else 0
    pat = re.compile(regex, flags)
    out = sorted(set(s for s in strings if pat.search(s)))
    return out


def section(title: str):
    print(f"\n{'=' * 70}")
    print(f"  {title}")
    print(f"{'=' * 70}")


def main() -> int:
    p = Path(sys.argv[1])
    data = p.read_bytes()
    strings = scan_strings(data)
    print(f"scanning: {p}\n{len(strings):,} strings\n")

    # All -dxXXX flags + special ones
    section("ALL -dxXXX FLAGS (DirectX engine launch options)")
    for s in grep(strings, r'(^|\s|=)-dx[a-z]+'):
        if len(s) < 80:
            print(f"  {s}")

    # The mysterious "letitsnow"
    section("LET-IT-SNOW (Easter egg / dev flag)")
    for s in grep(strings, r'letitsnow|snow|winter|christmas', ic=True):
        if len(s) < 100:
            print(f"  {s}")

    # CHEAT-related
    section("CHEAT INFRASTRUCTURE")
    for s in grep(strings, r'\bcheat\b', ic=True):
        if 5 <= len(s) <= 100:
            print(f"  {s}")

    # FreeCam
    section("FREECAM / FLY / NOCLIP")
    for s in grep(strings, r'\b(freecam|flycam|noclip|wireframe|fullbright|fly_)\b'):
        if len(s) < 100:
            print(f"  {s}")

    # First-person
    section("FIRST-PERSON MODE")
    for s in grep(strings, r'firstperson|first_person|fp_mode|firstmode'):
        if len(s) < 100:
            print(f"  {s}")

    # Give / Spawn / Kill commands
    section("GIVE / SPAWN COMMANDS")
    for s in grep(strings, r'^(Give|Spawn)[A-Z]'):
        if len(s) < 80:
            print(f"  {s}")

    # Teleport
    section("TELEPORT / WARP")
    for s in grep(strings, r'teleport|warp|levelhub', ic=True):
        if len(s) < 100:
            print(f"  {s}")

    # DEV / Developer
    section("DEVELOPER / INTERNAL FLAGS")
    for s in grep(strings, r'\b(developer|dev_mode|devmode|internal|dev[A-Z])'):
        if 5 <= len(s) <= 100:
            print(f"  {s}")

    # GodMode / Invincible / Health
    section("GODMODE / INVINCIBLE / HEALTH")
    for s in grep(strings, r'\b(godmode|invincib|invuln|infinite_health|infhealth)'):
        if len(s) < 100:
            print(f"  {s}")

    # Show / Hide / Toggle
    section("SHOW / HIDE / TOGGLE (debug visualisations)")
    for s in grep(strings, r'^(Show|Hide|Toggle)(Bbox|Triggers|Normals|Path|Coll|Hitbox|FPS|Wireframe|Nav|Light|Shadow|AI|Debug)'):
        if len(s) < 100:
            print(f"  {s}")

    # Verbosity / Log channels
    section("LOG CHANNELS / VERBOSITY")
    for s in grep(strings, r'(verbosity|loglevel|log_level|verbose|^[A-Z]+_LOG_)'):
        if len(s) < 80:
            print(f"  {s}")

    # Engine commands - typical syntax: "command<TAB>description"
    section("CONSOLE COMMAND CANDIDATES (command-name + description shape)")
    seen = set()
    for s in strings:
        # Pattern: lowercase_or_camelcase command name then space then a sentence
        m = re.match(r'^([a-z][a-zA-Z_0-9]{4,30})\s{2,}([A-Z].{8,80})$', s)
        if m:
            cmd, desc = m.group(1), m.group(2)
            if cmd not in seen:
                seen.add(cmd)
                if len(seen) <= 50:
                    print(f"  {cmd:<32} {desc}")

    # AI / Behaviour Tree debug
    section("AI / BEHAVIOR TREE DEBUG")
    for s in grep(strings, r'^Aicbt|^Aibt|behavior.?tree|^Ai[A-Z].*[Dd]ebug'):
        if len(s) < 100:
            print(f"  {s}")

    # Hidden modes / Hidden levels
    section("HIDDEN / SECRET / UNLOCK")
    for s in grep(strings, r'(secret|hidden|locked|unlock|unlockable)'):
        if 5 <= len(s) <= 100:
            print(f"  {s}")

    # Console keys / Bindings
    section("KEY BINDINGS / HOTKEYS")
    for s in grep(strings, r'(bindkey|keymap|keybind|^Bind|console_key|F[0-9]+_|tilde|backtick)'):
        if 5 <= len(s) <= 100:
            print(f"  {s}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

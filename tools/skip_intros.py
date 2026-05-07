"""Skip the pre-menu logo intros by renaming Bink files.

The game opens these on launch in sequence; renaming makes the open fail
and the game proceeds to the next state (in our testing this just goes
straight to the main menu without crashing). Restorable.

Targets (by filename, case-insensitive):
    BinkLegal.BIK   - RAD Bink legal notice
    legal.BIK       - generic legal notice
    dsny.BIK        - Disney logo
    avlogo.BIK      - Avalanche Software logo
    bvg.BIK         - Buena Vista Games logo

In-game cutscenes (egy, sti, dfi, end, credits, etc.) are NOT touched.

Usage:
    python tools\\skip_intros.py             # rename to .skip
    python tools\\skip_intros.py --restore   # restore from .skip
    python tools\\skip_intros.py --check     # report current state
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO     = Path(__file__).resolve().parent.parent
GAME_DIR = REPO / "Game"

INTRO_NAMES = {
    "BinkLegal.BIK",
    "legal.BIK",
    "dsny.BIK",
    "avlogo.BIK",
    "bvg.BIK",
}

SKIP_SUFFIX = ".intro_skip"


def find_targets() -> list[Path]:
    """Find all matching files by case-insensitive name match anywhere under Game/."""
    out = []
    if not GAME_DIR.exists():
        return out
    targets_lc = {n.lower() for n in INTRO_NAMES}
    for p in GAME_DIR.rglob("*.BIK"):
        if p.name.lower() in targets_lc:
            out.append(p)
    return out


def find_skipped() -> list[Path]:
    if not GAME_DIR.exists():
        return []
    return list(GAME_DIR.rglob(f"*{SKIP_SUFFIX}"))


def cmd_check():
    active   = find_targets()
    skipped  = find_skipped()
    print(f"intros active (will play):    {len(active)}")
    for p in active:
        print(f"  {p.relative_to(GAME_DIR)}  ({p.stat().st_size:,} B)")
    print(f"intros skipped (renamed):     {len(skipped)}")
    for p in skipped:
        print(f"  {p.relative_to(GAME_DIR)}")


def cmd_skip():
    targets = find_targets()
    if not targets:
        print("no intro files to skip (already done?)")
        return
    for p in targets:
        new = p.with_suffix(p.suffix + SKIP_SUFFIX)
        if new.exists():
            print(f"  [skip] {new.name} already exists, leaving {p.name}")
            continue
        p.rename(new)
        print(f"  [ok] {p.name} -> {new.name}")


def cmd_restore():
    skipped = find_skipped()
    if not skipped:
        print("no skipped intros to restore")
        return
    for p in skipped:
        # Strip .intro_skip
        if not p.name.endswith(SKIP_SUFFIX):
            continue
        original_name = p.name[:-len(SKIP_SUFFIX)]
        original = p.with_name(original_name)
        if original.exists():
            print(f"  [skip] {original.name} already present, leaving {p.name}")
            continue
        p.rename(original)
        print(f"  [ok] {p.name} -> {original.name}")


def main():
    p = argparse.ArgumentParser()
    g = p.add_mutually_exclusive_group()
    g.add_argument("--check",   action="store_true")
    g.add_argument("--restore", action="store_true")
    args = p.parse_args()

    if args.check:
        cmd_check()
    elif args.restore:
        cmd_restore()
    else:
        cmd_skip()


if __name__ == "__main__":
    main()

"""
Analyze sprite_probe CSV captures to find a menu-vs-HUD classifier.

State labels:
  GAMEPLAY_HUD             - HUD only (in-game, no menu open)
  MAIN_MENU                - menu only (no HUD)
  GAMEPLAY_TIP_WINDOW      - HUD + tip overlay (CRITICAL: coexistence test)
  GAMEPLAY_ESC_MENU        - HUD + escape menu
"""
import csv
import glob
import os
from collections import defaultdict, Counter

CAPTURE_DIR = r"d:/Projects/Programming/MeetTheRobinsons/Game"
PATTERN     = "mtr-asi-sprite-probe_*.csv"


def load(path):
    with open(path, newline="") as f:
        rdr = csv.DictReader(f)
        return list(rdr)


def state_key_set(rows):
    return set(int(r["state_key"], 16) for r in rows)


def main():
    files = sorted(glob.glob(os.path.join(CAPTURE_DIR, PATTERN)))
    captures = {}
    for fp in files:
        name = os.path.basename(fp).replace("mtr-asi-sprite-probe_", "").replace(".csv", "")
        captures[name] = load(fp)

    # Get top_screen + stack_depth for first row of each capture (sanity).
    print("=== top_screen seen during each capture ===")
    for name, rows in captures.items():
        if not rows:
            continue
        first = rows[0]
        # also check the unique top_screens in the capture
        screens = Counter(r["top_screen"] for r in rows)
        print(f"  {name:30s}  depth={first['stack_depth']}  "
              f"first_top={first['top_screen']!r:25s}  "
              f"screens={dict(screens)}")
    print()

    # Per-capture: per-state-key bucket of sort_keys it appears with.
    # Distinguish "always-HUD" keys (tied to HUD render) from "always-MENU" keys.
    hud      = captures.get("GAMEPLAY_HUD", [])
    menu     = captures.get("MAIN_MENU", [])
    tip      = captures.get("GAMEPLAY_TIP_WINDOW", [])
    esc      = captures.get("GAMEPLAY_ESC_MENU", [])
    esc_map  = captures.get("GAMEPLAY_ESC_MENU_map", [])
    esc_mis  = captures.get("GAMEPLAY_ESC_MENU_mission", [])
    options  = captures.get("MAIN_MENU_options", [])
    cheats   = captures.get("MAIN_MENU_2_cheats", [])

    hud_keys   = state_key_set(hud)
    menu_keys  = state_key_set(menu)
    tip_keys   = state_key_set(tip)
    esc_keys   = state_key_set(esc)

    print("=== HUD keys (12 total) — appear in GAMEPLAY_HUD capture ===")
    hud_sks = defaultdict(list)
    for r in hud:
        hud_sks[int(r["state_key"], 16)].append(int(r["sort_key"]))
    for k in sorted(hud_keys):
        sk_set = sorted(set(hud_sks[k]))
        sk_str = (str(sk_set) if len(sk_set) <= 6
                  else f"[{sk_set[0]}..{sk_set[-1]} +{len(sk_set)-2} more]")
        print(f"  0x{k:08X}  count={len(hud_sks[k]):>5}  sort_keys={sk_str}")
    print()

    print("=== MAIN_MENU keys (8 total) ===")
    menu_sks = defaultdict(list)
    for r in menu:
        menu_sks[int(r["state_key"], 16)].append(int(r["sort_key"]))
    for k in sorted(menu_keys):
        sk_set = sorted(set(menu_sks[k]))
        sk_str = (str(sk_set) if len(sk_set) <= 6
                  else f"[{sk_set[0]}..{sk_set[-1]} +{len(sk_set)-2} more]")
        print(f"  0x{k:08X}  count={len(menu_sks[k]):>5}  sort_keys={sk_str}")
    print()

    # The CRITICAL test: TIP_WINDOW — what are the 3 NEW keys (vs HUD)?
    # And what are the 10 ESC-only keys?
    if tip_keys and hud_keys:
        tip_only = tip_keys - hud_keys
        tip_sks = defaultdict(list)
        for r in tip:
            tip_sks[int(r["state_key"], 16)].append(int(r["sort_key"]))
        print(f"=== TIP-only keys (in TIP, NOT in HUD) — {len(tip_only)} keys ===")
        for k in sorted(tip_only):
            sk_set = sorted(set(tip_sks[k]))
            sk_str = (str(sk_set) if len(sk_set) <= 6
                      else f"[{sk_set[0]}..{sk_set[-1]} +{len(sk_set)-2} more]")
            print(f"  0x{k:08X}  count={len(tip_sks[k]):>5}  sort_keys={sk_str}")
        print()

    if esc_keys and hud_keys:
        esc_only = esc_keys - hud_keys
        esc_sks = defaultdict(list)
        for r in esc:
            esc_sks[int(r["state_key"], 16)].append(int(r["sort_key"]))
        print(f"=== ESC-only keys (in ESC_MENU, NOT in HUD) — {len(esc_only)} keys ===")
        for k in sorted(esc_only):
            sk_set = sorted(set(esc_sks[k]))
            sk_str = (str(sk_set) if len(sk_set) <= 6
                      else f"[{sk_set[0]}..{sk_set[-1]} +{len(sk_set)-2} more]")
            print(f"  0x{k:08X}  count={len(esc_sks[k]):>5}  sort_keys={sk_str}")
        print()

    # Stability check: are the same state_keys used in MAIN_MENU vs MAIN_MENU_options
    # vs MAIN_MENU_2_cheats? If state_key is texture-pointer-bound, repeated
    # captures of the same screen should hit the same set.
    print("=== state_key stability across captures of same screen ===")
    main_menu_caps = [(n, r) for (n, r) in captures.items() if n.startswith("MAIN_MENU")]
    if len(main_menu_caps) > 1:
        all_keys = [(n, state_key_set(r)) for (n, r) in main_menu_caps]
        for n, ks in all_keys:
            print(f"  {n:30s}  {len(ks)} keys  {sorted(f'0x{k:08X}' for k in ks)}")
    print()

    # Position bucket — for each capture, is there a clean separation in
    # p0_x / p0_y (vert 0) for HUD vs menu?
    print("=== position distribution (p0_x, p0_y) — quartiles ===")
    for name, rows in captures.items():
        if not rows:
            continue
        xs = sorted(float(r["p0_x"]) for r in rows)
        ys = sorted(float(r["p0_y"]) for r in rows)
        n = len(xs)
        q = lambda arr, p: arr[int(n * p)] if n > 0 else 0.0
        print(f"  {name:30s}  "
              f"x[min={xs[0]:.3f}, q25={q(xs,0.25):.3f}, q50={q(xs,0.50):.3f}, q75={q(xs,0.75):.3f}, max={xs[-1]:.3f}]  "
              f"y[min={ys[0]:.3f}, q50={q(ys,0.50):.3f}, max={ys[-1]:.3f}]")


if __name__ == "__main__":
    main()

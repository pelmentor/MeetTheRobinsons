"""Reset state, deploy fresh mtr-asi.asi, optionally launch the game.

Replaces the PowerShell reset-deploy script with a single Python entry point.
Uses ctypes for Windows API calls (no extra deps).

Modes (mutually exclusive; default = direct):
    --direct      kill stale state, deploy, launch Wilbur.exe directly (no launcher)
    --launcher    kill stale state, deploy, launch via Launcher.exe
    --clean       kill stale state only (no deploy, no launch)
    --no-launch   deploy only

Examples:
    python tools\\run_game.py                # default: direct
    python tools\\run_game.py --launcher
    python tools\\run_game.py --clean
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

# Reuse the Mutant scanner.
sys.path.insert(0, str(Path(__file__).parent))
from find_mutex_holder import (
    query_system_handles,
    find_mutant_type_index,
    get_handle_objectname,
    proc_image_name,
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX,
    PROCESS_DUP_HANDLE,
    PROCESS_QUERY_LIMITED_INFO,
    DUPLICATE_SAME_ACCESS,
    kernel32,
    ntdll,
)

REPO     = Path(__file__).resolve().parent.parent
GAME_DIR = REPO / "Game"
BUILD    = REPO / "src" / "mtr-asi" / "build"
WILBUR   = GAME_DIR / "Wilbur.exe"
LAUNCHER = GAME_DIR / "Launcher.exe"
GAME_ASI = GAME_DIR / "mtr-asi.asi"

MUTEX_NEEDLE = "Disney_s_Meet_The_Robinsons"


# -------- pretty output --------

def _color(c, msg): return f"\033[{c}m{msg}\033[0m"
def step(msg): print(_color("36;1", f"==> {msg}"))
def ok(msg):   print(_color("32",   f"    [ok] {msg}"))
def warn(msg): print(_color("33",   f"    [warn] {msg}"))
def err(msg):  print(_color("31",   f"    [err] {msg}"))


# -------- process / mutex ops --------

PROCESS_TERMINATE = 0x0001

kernel32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
kernel32.TerminateProcess.restype  = wt.BOOL

def kill_pid(pid: int) -> bool:
    h = kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if not h:
        return False
    try:
        return bool(kernel32.TerminateProcess(h, 1))
    finally:
        kernel32.CloseHandle(h)

def find_processes_by_image(images: list[str]) -> list[tuple[int, str]]:
    """Walk all PIDs via OpenProcess+GetProcessImageFileName. Returns matching."""
    # Use enum technique: open every reasonable PID; psapi has EnumProcesses.
    arr_size = 4096
    while True:
        arr = (wt.DWORD * arr_size)()
        cb_needed = wt.DWORD()
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        psapi.EnumProcesses.argtypes = [ctypes.POINTER(wt.DWORD), wt.DWORD, ctypes.POINTER(wt.DWORD)]
        psapi.EnumProcesses.restype  = wt.BOOL
        if not psapi.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(cb_needed)):
            return []
        n = cb_needed.value // ctypes.sizeof(wt.DWORD)
        if n < arr_size:
            break
        arr_size *= 2
    results = []
    images_lc = [img.lower() for img in images]
    for i in range(n):
        pid = arr[i]
        if pid == 0:
            continue
        img = proc_image_name(pid)
        if not img.startswith("<"):
            base = os.path.basename(img).lower()
            if base in images_lc:
                results.append((pid, img))
    return results

def find_mutex_holders(needle: str) -> list[tuple[int, str]]:
    """Return list of (pid, image) holding a mutex whose name contains needle."""
    mutant_idx = find_mutant_type_index()
    buf, addr, n, base_off = query_system_handles()
    entry_size = ctypes.sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX)
    me = kernel32.GetCurrentProcess()
    procs: dict[int, int] = {}
    holders = []

    def get_proc(pid: int) -> int:
        if pid in procs:
            return procs[pid]
        h = kernel32.OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFO, False, pid)
        procs[pid] = h or 0
        return procs[pid]

    needle_lc = needle.lower()
    for i in range(n):
        e = SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX.from_address(addr + base_off + i * entry_size)
        if e.ObjectTypeIndex != mutant_idx:
            continue
        pid = int(e.UniqueProcessId)
        if pid in (0, 4):
            continue
        ph = get_proc(pid)
        if not ph:
            continue
        dup = wt.HANDLE(0)
        ok_dup = kernel32.DuplicateHandle(ph, int(e.HandleValue), me, ctypes.byref(dup), 0, False, DUPLICATE_SAME_ACCESS)
        if not ok_dup or not dup.value:
            continue
        try:
            name = get_handle_objectname(dup.value)
            if name and needle_lc in name.lower():
                holders.append((pid, proc_image_name(pid)))
        finally:
            kernel32.CloseHandle(dup.value)

    for h in procs.values():
        if h:
            kernel32.CloseHandle(h)
    return holders


# -------- file ops --------

def is_locked(path: Path) -> bool:
    if not path.exists():
        return False
    try:
        with open(path, "ab"):
            return False
    except (PermissionError, OSError):
        return True

def free_locked_asi(path: Path):
    if not is_locked(path):
        ok(f"{path.name} is free")
        return
    ts = time.strftime("%Y%m%d_%H%M%S")
    bak = path.with_suffix(path.suffix + f".zombie-{ts}")
    try:
        os.rename(path, bak)
        warn(f"asi was locked, renamed -> {bak.name}")
    except OSError as e:
        err(f"rename-trick failed: {e}")


# -------- main flow --------

def cmd_clean():
    step("Killing live Wilbur/Launcher")
    procs = find_processes_by_image(["Wilbur.exe", "Launcher.exe"])
    if procs:
        for pid, img in procs:
            if kill_pid(pid):
                warn(f"killed {os.path.basename(img)} [{pid}]")
            else:
                err(f"could not kill pid {pid}")
        time.sleep(0.3)
    else:
        ok("no live processes")

    step(f"Finding holders of *{MUTEX_NEEDLE}*")
    holders = find_mutex_holders(MUTEX_NEEDLE)
    if holders:
        for pid, img in holders:
            warn(f"holder: {os.path.basename(img)} [{pid}] -> killing")
            if kill_pid(pid):
                ok(f"killed pid {pid}")
            else:
                err(f"failed to kill pid {pid}")
        time.sleep(0.5)
        # re-check
        holders2 = find_mutex_holders(MUTEX_NEEDLE)
        if holders2:
            warn(f"{len(holders2)} holder(s) still alive (probably privileged)")
        else:
            ok("mutex cleared")
    else:
        ok("no holders")

    step(f"Ensuring {GAME_ASI.name} is writable")
    free_locked_asi(GAME_ASI)


def cmd_deploy(build_cfg: str = "Release"):
    src = BUILD / build_cfg / "mtr-asi.asi"
    if not src.exists():
        err(f"build artifact not found: {src}")
        err(f"run: cmake --build {BUILD} --config {build_cfg}")
        return False
    step(f"Deploying {src.name}")
    shutil.copy2(src, GAME_ASI)
    info = GAME_ASI.stat()
    ok(f"deployed {GAME_ASI.name} ({info.st_size:,} bytes)")
    return True


def cmd_launch_direct():
    step("Starting Wilbur.exe directly (cmdline hook fills -dxresolution=native)")
    if not WILBUR.exists():
        err(f"not found: {WILBUR}")
        return
    args = [str(WILBUR), "-dxfullscreen", "-dxadapter=0", "-launchit"]
    subprocess.Popen(args, cwd=str(GAME_DIR))
    ok(f"launched: {' '.join(args[1:])}")


def cmd_launch_launcher():
    step("Starting via Launcher.exe")
    if not LAUNCHER.exists():
        err(f"not found: {LAUNCHER}")
        return
    subprocess.Popen([str(LAUNCHER)], cwd=str(GAME_DIR))
    ok("launcher started")


def main():
    p = argparse.ArgumentParser(description="Reset/deploy/run the mod.")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--direct",    action="store_true", help="(default) launch Wilbur directly")
    g.add_argument("--launcher",  action="store_true", help="launch via Launcher.exe")
    g.add_argument("--clean",     action="store_true", help="only clean state, no deploy or launch")
    g.add_argument("--no-launch", action="store_true", help="deploy only")
    p.add_argument("--build", default="Release", help="cmake config name (default Release)")
    args = p.parse_args()

    # enable ANSI on Windows
    try:
        kernel32.SetConsoleMode.argtypes = [wt.HANDLE, wt.DWORD]
        kernel32.SetConsoleMode.restype = wt.BOOL
        h_out = kernel32.GetStdHandle(-11)
        mode = wt.DWORD()
        kernel32.GetConsoleMode(h_out, ctypes.byref(mode))
        kernel32.SetConsoleMode(h_out, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
    except Exception:
        pass

    cmd_clean()

    if args.clean:
        return

    if not cmd_deploy(args.build):
        sys.exit(1)

    if args.no_launch:
        return
    if args.launcher:
        cmd_launch_launcher()
    else:
        cmd_launch_direct()


if __name__ == "__main__":
    main()

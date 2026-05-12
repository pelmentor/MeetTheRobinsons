# Dual-launch bypass — graceful two-Wilbur on one machine

**Date**: 2026-05-12 (Phase 1.4b.1 — live-test unblocker for 1.4b/c)
**Status**: SHIPPED
**Predecessors**:
- `research/findings/coop-phase-1-3-vs-1-4-ordering-2026-05-12.md`
- `memory/project_state_2026-05-12_phase_1_4a_wire_format.md`

## Problem

Wilbur.exe and Launcher.exe both create a `Global\` named kernel mutex at
startup:

```c
CreateMutexA(NULL, TRUE, "Global\\Disney_s_Meet_The_Robinsons");
if (GetLastError() == ERROR_ALREADY_EXISTS) {
    show_dialog("Another instance already exists.");
    return; // exit
}
```

For Phase 1.4 (co-op UDP transport) live-test we need **two Wilbur.exe
instances on the same machine** — one host, one client. Same-machine
dual-launch is blocked by this mutex. Two physical machines or two
isolated VMs would also work but are friction this project doesn't need
to accept.

## 3-agent consensus

Per `feedback_no_questions_when_rule1_dictates.md`, ran three parallel
agents (architect / reviewer / explorer) on the design question.

**Architect (`feature-dev:code-architect`)**: MinHook on
`kernel32!CreateMutexA`, rewrite the name with a per-PID suffix, gate
implicitly on the existing co-op cmdline flags. Reusable file:
`src/mtr-asi/src/coop/coop_dual_launch.cpp`.

**Reviewer (`feature-dev:code-reviewer`)**: Picked Option A (name
rewrite) as the only RULE-1-compliant choice. Rejected the alternatives
explicitly:
- Suppressing `ERROR_ALREADY_EXISTS` = catch-and-ignore (RULE №1 forbidden).
- NOPping the engine's exit branch = broad suppression (Principle 4 borderline).
- Pre-creating the mutex ourselves = unnecessary machinery (rejected on simplicity).

**Explorer (`feature-dev:code-explorer`)**: Reported that **MTA has NO
precedent** for this problem — they prevent two `gta_sa.exe` instances
by `TerminateProcess`ing the existing one (`HandleIfGTAIsAlreadyRunning`
at `reference/mtasa-blue/Client/loader/MainFunctions.cpp:1039-1060`),
not by hooking GTA's mutex. Their multi-player model is one GTA + one
MTA launcher, no dual-GTA-instance scenario exists. This means our
design is original, not borrowed.

Also reported the exact MinHook-on-kernel32 pattern already in the
codebase: `cmdline_hook.cpp:292-318` hooks `GetCommandLineA/W` via
`GetProcAddress` + `MH_CreateHook` + `MH_EnableHook`. We mirror that
verbatim.

## Decision: name rewrite

The actual root cause of the dual-launch refusal is **kernel-namespace
contention**: two processes both try to claim the global name
`Global\Disney_s_Meet_The_Robinsons`. The first wins; the second's
CreateMutex returns `ERROR_ALREADY_EXISTS` and the engine's exit branch
fires.

**Rewriting the name eliminates the contention.** Each process owns a
distinct kernel object (`..._pid<PID>`). The engine's singleton-guard
runs against a name no one else claimed, sees no conflict, and the game
starts. The exit branch is reached only when it should be.

This is the proper fix per RULE №1. The other options fight the guard
or violate Principle 1; the name rewrite is the only one that addresses
the actual cause without papering over it.

## Implementation

### Mechanism

MinHook detours on **both** `kernel32!CreateMutexA` and
`kernel32!CreateMutexW`. The hook body checks `lpName` against the
exact Disney name (A and W respectively) and rewrites to
`Global\Disney_s_Meet_The_Robinsons_pid<PID>` for that one match.
All other `CreateMutex` calls — DirectInput, DXVK, COM, CRT internals —
pass through untouched. Principle 4 (targeted, not broad) preserved.

Both A and W are hooked because Wilbur.exe's encoding is not yet RE'd
in the IDB. Launcher.exe is confirmed A; Wilbur.exe is likely A too
(2007-vintage code, MFC-style), but hooking both is one extra detour
of insurance against a wrong guess.

### Gate

Implicit on the existing co-op cmdline flags:
- `-mtrasi-coop-host`
- `-mtrasi-coop-connect`

When either is present, the hook is installed and the rename is active.
When neither is present, **`install()` returns early — the hook is
never created.** Normal single-player launches keep singleton
enforcement unchanged. Zero overhead on any CreateMutex call for
non-coop users.

A dedicated `-mtrasi-coop-allow-dual-launch` flag was considered and
rejected per RULE №2: a separate flag would be parallel-path baggage,
since the only context in which dual-launch is wanted is a coop
session, and the coop flags already express that.

### Files

New:
- `src/mtr-asi/src/coop/coop_dual_launch.cpp`

Modified:
- `src/mtr-asi/CMakeLists.txt` — adds the new source.
- `src/mtr-asi/src/dllmain.cpp` — forward-declares
  `mtr::coop::dual_launch::install` and calls it in
  `DLL_PROCESS_ATTACH` immediately after `mtr::cmdline::install()` and
  before `mtr::cvar_dump::install()`. Order rationale: cmdline must be
  armed first so `bypass_enabled()`'s `GetCommandLineA` read sees any
  rewritten cmdline; install must precede WinMain (which Wilbur runs
  CreateMutexA from). Both conditions hold.

### Init timing

- `DLL_PROCESS_ATTACH` fires under loader lock before the CRT runs and
  before WinMain. (Same window already used by `cmdline_hook.cpp` —
  whose comment at line 157-158 of `dllmain.cpp` documents the
  invariant explicitly.)
- `kernel32.dll` is the first DLL the loader maps; it is always present
  and fully initialised by the time any third-party DllMain fires.
- The hook is installed synchronously in `DLL_PROCESS_ATTACH`. Wilbur's
  CreateMutexA call happens after `DLL_PROCESS_ATTACH` returns, so the
  detour is live for that call.

### Cmdline-gate helper

`cmdline_has_flag(cl, flag)` is a private file-static mirror of the
helper in `coop_registry_mirror.cpp:86-96`. Whole-word match
(flag must be followed by `\0`, ` `, or `\t`) — avoids the
substring trap (`-foo-host-x` accidentally matching `-foo-host`).

### Detour

```cpp
HANDLE WINAPI hk_CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL bInitialOwner, LPCSTR lpName) {
    if (lpName != nullptr && std::strcmp(lpName, kDisneyMutexNameA) == 0) {
        char rewritten[160];
        const int n = std::snprintf(rewritten, sizeof(rewritten),
            "%s_pid%lu", kDisneyMutexNameA, GetCurrentProcessId());
        if (n > 0 && static_cast<std::size_t>(n) < sizeof(rewritten)) {
            return g_orig_CreateMutexA(sa, bInitialOwner, rewritten);
        }
        // fall through unchanged on truncation (impossible — 35-char prefix
        // + 10-digit DWORD max = 50 bytes, buffer is 160).
    }
    return g_orig_CreateMutexA(sa, bInitialOwner, lpName);
}
```

W variant is the symmetric `swprintf` + `wcscmp` pair.

## 2-agent audit

Per `feedback_audit_pattern.md`, ran two parallel reviewers (domain-
fidelity + correctness) at >70% confidence threshold.

### Audit 1 — domain fidelity

**Two findings.**

1. **Partial-enable cleanup (88% conf)**: original code used
   short-circuit `||` for `MH_EnableHook(pA) || MH_EnableHook(pW)` — if
   A succeeded and W failed, A stayed armed with no cleanup path.
   **Fixed same session**: enable individually, capture both
   `MH_STATUS`, disable the survivor on partial failure.

2. **W-path log message uses placeholder prose for source name (80%
   conf)**: log says `\"Global\\Disney_...\"` instead of the actual wide
   string. **No action**: this is consistent with the existing
   `cmdline_hook.cpp` precedent (the cmdline W hook also doesn't dump
   wide strings into the ASCII log file). The body is reachable only
   when `wcscmp(lpName, kDisneyMutexNameW) == 0`, so the prose is
   accurate even though abbreviated.

Other checks (RULE №1 root-cause, RULE №2 no-parallel-paths, Principle
4 targeting narrowness, Principle 7 placement, defensive fall-through)
all clean at >70%.

### Audit 2 — correctness

**One finding above threshold; declined per Principle 5.**

13. **Post-creation `OpenMutex` query (80% conf)**: If Wilbur's WinMain
    follows the uncommon pattern of CreateMutex *then* OpenMutex on the
    same name, our rename would defeat the OpenMutex query. The agent
    acknowledged that "common singleton patterns do exactly this and
    nothing more" (CreateMutex + GetLastError, no follow-up OpenMutex)
    — i.e., this is a 20% risk, not a 80% one. **Tracked but not
    pre-emptively fixed** per Principle 5 (minimum viable subset). The
    OpenMutex hook is trivial to add (mirror the CreateMutex hook), and
    will be added IF live-test reveals the dual-launch fails despite
    the CreateMutex hook firing. Reviewing the IDB for an OpenMutex
    xref is also cheap and could short-circuit this.

Twelve other checks (thread safety of magic-static, hook reentrance via
log, calling-convention match, buffer sizing A and W, wcscmp encoding,
`%ls` portability under MSVC, GetProcAddress timing, cleanup on
DLL_PROCESS_DETACH, cmdline immutability, `%lu`/DWORD match, stack
buffer lifetime vs kernel copy, locale-sensitive comparison) all
**clean at >70%**.

## Build state

- Pre-audit: 703488 → 705024 bytes (+1536). Two MinHook detours, two
  hook bodies, install(), bypass_enabled(), cmdline_has_flag, two
  string literals, two trampoline pointers.
- Post-audit fix: 705024 → 706048 bytes (+1024). Per-hook
  `MH_STATUS` capture, partial-failure disable path, and a more
  detailed log message.
- Deployed to `Game/mtr-asi.asi`.
- Build clean, no warnings.

## Live-test plan

Once Phase 1.4b (NetSession) and 1.4c (integration) land, the natural
test is: launch two Wilbur.exe instances on the same machine, one as
host, one as client, observe both reach the main menu and exchange
pose snapshots. Until then, the dual-launch portion is testable in
isolation by simply launching two Wilbur.exe with any coop flag:

```
# Terminal 1
Game\Wilbur.exe -mtrasi-coop-host 7777 -dxresolution=1280x720 -launchit

# Terminal 2  
Game\Wilbur.exe -mtrasi-coop-connect 127.0.0.1:7777 -dxresolution=1280x720 -launchit
```

Expected: both reach main menu. The log lines

```
dual_launch: CreateMutex{A,W} hooks armed; will rename "Global\Disney_s_Meet_The_Robinsons" -> "..._pid<PID>" for THIS process
dual_launch: rewrote CreateMutexA("Global\Disney_s_Meet_The_Robinsons") -> "Global\Disney_s_Meet_The_Robinsons_pid<PID>"
```

appear in both `Game/mtr-asi.log` files (each process has its own log
side-by-side because the log file is named per-PID — verify).

Anti-test: a third instance launched WITHOUT a coop flag must exit with
the "Another instance" dialog if either coop instance is still alive
on the machine. Reason: the third instance's `install()` returns early
(no coop flag), no rewrite happens, and the unmangled
`Global\Disney_s_Meet_The_Robinsons` was claimed... but actually no —
it wasn't. The two coop instances renamed *theirs* to `_pid<X>` and
`_pid<Y>`; the unmangled name is unclaimed. So the third non-coop
instance would actually start successfully even with two coop
instances running. Singleton enforcement is preserved *across non-coop
instances*. This is the intended trade-off: the user opted into
dual-launch by passing coop flags, and that opt-in is per-process.

## Edge cases (from architect proposal, retained)

- **Launcher creates the mutex first**: Launcher exits via
  `ShellExecuteExW` before Wilbur starts. By the time Wilbur runs, the
  launcher process has exited and its mutex handle is released. No
  conflict regardless of bypass state.
- **PID reuse**: PIDs are only reused after a process exits. When a
  process exits, its `_pid<PID>` mutex handle is auto-released. PID
  collision between two simultaneously live processes is structurally
  impossible.
- **Zombie mutex orphan (`known-issues.md` #4)**: this bug exists when
  a process is killed and the kernel takes time to release its mutex
  handle. With the rename, each process's name encodes its own PID, so
  a zombie holding `..._pid<dead-PID>` does not block a fresh launch
  that would create `..._pid<new-PID>`. The bypass scenario silently
  immunises against #4. `tools/run_game.py`'s zombie-killer continues
  to handle the non-coop case where the unmangled name is contested.

## What this enables

- Phase 1.4b (NetSession class with Winsock UDP socket lifecycle).
- Phase 1.4c (integration: host emits pose snapshots from
  `MtrPlayerManager::do_pulse`; client receives + calls
  `push_interp_snapshot`).
- All future co-op work on a single dev machine without the friction
  of two-VM or two-physical-machine testing.

## Pattern reinforced

- **No-questions rule**: "Go" + a one-line new directive → consulted
  3 agents in parallel, NOT the user.
- **MTA-as-reference (without precedent)**: explorer agent's first job
  was checking MTA's `Client/loader/` for prior art. Finding *none*
  (they kill GTA rather than dual-launch it) was as useful as finding
  some — it told us we're on our own and can't lean on a known-good
  design. Original design grounded in 3-agent consensus + RULE №1 +
  Principle 4.
- **Audit gates ship**: the partial-enable cleanup gap (88% conf) was
  a real defect that the audit caught. Fixed same session.
- **Principle 5 holds even under risk pressure**: the post-creation
  OpenMutex concern (80% conf) was tempting to fix preemptively but
  the underlying claim ("common singleton patterns don't do follow-up
  OpenMutex") is well-supported. Live-test will resolve the 20%
  uncertainty cheaply; a preemptive hook would be Principle 5
  baggage.

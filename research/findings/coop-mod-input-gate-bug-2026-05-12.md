# Bug: Mod features (F3, MMB-tp) don't work on client window

**Date observed**: 2026-05-12, during user manual LAN test post-fix
**Phase**: 1.7 follow-up (input-gate refactor)
**Status**: Diagnosed. Fix pending next session.

## Symptom

On the user's LAN test, the host window's mod features all worked
(F2 menu, F3 / FreeCam, MMB-tp = middle-mouse-button teleport, etc.).
On the **client** window, the same hotkeys did nothing — pressing
MMB while pointing somewhere did not teleport the local Wilbur,
pressing F3 didn't toggle FreeCam.

## Root cause analysis

We installed two foreground-gates in 2026-05-12 to prevent input
bleeding between the two locally-running Wilbur windows (`coop-host`
and `coop-client` BATs):

1. **`hk_SetCooperativeLevel`** (`src/mtr-asi/src/dinput_hook.cpp`) —
   strips `DISCL_FOREGROUND | DISCL_EXCLUSIVE`, substitutes
   `DISCL_NONEXCLUSIVE | DISCL_BACKGROUND` so the OS doesn't grab
   exclusive input for whichever window registered first.

2. **Mouse foreground gate in `hk_GetDeviceState`** (same file) —
   after the splash screen is past (latched via
   `screen_push::current_top_name()` returning a known top), checks
   `GetForegroundWindow()` per-call. If the foreground window's PID
   is not this process, zeros the entire `DIMOUSESTATE` buffer
   before returning to the engine.

The mouse gate's intent was to make sure the **engine's** input poll
got "no input" when its window was in the background, so the engine
doesn't try to move Wilbur or fire weapons from clicks meant for the
other window. The flaw: the gate zeros the buffer for **all DI mouse
consumers**, not just the engine. Mod-side hooks (`menu`'s ImGui poll
adapter, FreeCam toggle, MMB-tp picker) read from the same gated DI
device.

So when the user's client window was in the background (which it
often was while clicking between windows), MMB and friends were
zeroed before reaching the mod's processing path.

The same may affect keyboard (F3): need to verify. The DI keyboard
gate has a similar branch but may not be triggering for hotkeys —
if F3 is read via `GetAsyncKeyState` it's unaffected; if via DI
poll it's affected. Both paths exist in the codebase historically;
need to grep for the F3 read site.

## Why this is a RULE №1 issue, not a quick-fix

The bug is not "F3 sometimes doesn't fire" — it's a design mistake.
We applied the gate at the wrong layer. Mod features should ALWAYS
respond to input (it's debug tooling; the user wants to drive the
mod menu and the teleport regardless of whether the game window
"has focus" in OS terms). The gate is needed to keep the **engine
itself** from reading input that wasn't meant for it.

The proper fix is to **split the DI-read interception** by
consumer:

- **Mod-side hooks** read the unmodified DI buffer directly.
- **Engine-side polls** read a foreground-gated version.

This is principle 7 (engine-wrapper layer vs gameplay/network layer
boundary, adapted: engine-side DI vs mod-side DI).

## Fix plan (Phase 1.7-ish)

### Option (a) — separate read paths

Don't gate inside `hk_GetDeviceState`. Instead, the mod's own input
adapter (which currently reads from the DI device via the same hook
path) reads BEFORE the gate is applied, then the engine's call goes
through the gate.

Concretely: the mod menu / FreeCam toggle / MMB-tp picker should
have their own DI-buffer-cache (filled by a PRE hook on
`GetDeviceState` that reads the buffer once per frame). The mod
reads this cache directly. The engine's call (which is post-mod in
the call chain) goes through the existing `hk_GetDeviceState` with
the gate.

Risk: hard to identify "this call is engine-side, that call is
mod-side" inside the hook (the hook is a single function called by
many sites). Need to use the return address or a thread-local flag
set by the mod-side reader.

### Option (b) — gate inverted

Keep the gate, but **don't apply it to the buffer returned to mod
code**. The mod code reads via a wrapper that takes the un-gated
buffer (cached pre-gate). The engine sees the gated buffer.

Effectively the same as (a) but framed as "mod reads from cache,
engine reads from gated".

### Option (c) — move features to `GetAsyncKeyState` / WH_MOUSE_LL

Convert mod-side input (F2, F3, MMB-tp, etc.) to read input via
WinAPI rather than DI. WinAPI (`GetAsyncKeyState`,
`GetCursorPos`, `WH_MOUSE_LL`) is per-OS-window and doesn't go
through DI cooperative levels. The MMB-tp picker already has a
`WH_MOUSE_LL` path (from 2026-05-06 memory:
"`project_input_directinput_mouse.md`"). The pattern is there;
extend it to keyboard hotkeys.

### Recommended: (c) for hotkeys + (a) for ImGui menu adapter

Rationale:
- Hotkeys are individual one-shot reads. WinAPI is cleaner for
  this — no buffer caching, no DI cooperative-level dance.
- ImGui menu needs continuous key/mouse state for text input and
  drag — best served by a proper DI cache shared between mod-side
  reads and engine-side gated reads.

This puts the fix's complexity where it earns it (menu) and the
simple fix where it's enough (one-shot hotkeys).

## RE TODOs

### TODO 1 — locate F3 read site in current mod code

Grep `src/mtr-asi` for `F3` / `VK_F3` / `DIK_F3` to find where the
mod reads F3. Determine whether it uses DI poll or `GetAsyncKeyState`.

### TODO 2 — locate MMB-tp read site

Grep for `MMB` / `VK_MBUTTON` / `DIMOFS_BUTTON2` and similar. The
fix from 2026-05-09 (`project_mmb_teleport_root_cause_fix`) was on
the engine-side teleport handler, NOT the mod-side input reader
(presumably). Confirm where the mod-side reads MMB.

### TODO 3 — measure: which features are DI-only

After RE'ing, enumerate which mod features currently depend on the
DI hook for input. Plan WinAPI fallback for each.

### TODO 4 — confirm keyboard gate behavior

Read the current `hk_GetDeviceState` keyboard branch. If keyboard
buffer also gets zeroed when not foreground, F3 fails for the same
reason as MMB. If keyboard branch doesn't gate, F3 is failing for a
DIFFERENT reason and needs separate diagnosis.

## What this is NOT

- Not a fundamental bug in coop mod design.
- Not blocking coop functionality (game still runs, replication
  still works, only DEV-facing mod features are affected).
- Not a regression in shipped engine fixes (MMB-tp altitude fix
  from 2026-05-09 is unaffected; the bug is at a higher input layer).

## Priority

Medium. Annoying but doesn't block coop testing — workaround for
user is to click on the client window first to give it focus, then
press the hotkey. For testing-velocity reasons, fix in the same
session as Phase 1.7 (level-transition O4 fix) — both are quick
input-path-touch fixes that batch well.

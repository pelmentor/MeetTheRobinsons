# Screen system & state machine — RE complete (2026-05-05)

This is the engine's UI/screen-stack architecture in retail Wilbur.exe. We mapped it end-to-end while attempting to activate the unused `DEVMENU` screen. DEVMENU itself is unreachable (see "DEVMENU dead-end" below), but the rest of the system is cleanly RE'd and we have working hooks for screen-push capture and state-machine transition.

## Architecture

Three layers, in order from low to high:

1. **Property store** (`props_*` API) — flat key→value string store. Used for tunables that span screens (e.g. `TargetGameLevel`, `BackupSaveGameAct`).
2. **State machine** (`state_machine_*` API) — singleton at `g_state_machine_ptr` (`0x728A30`) with a "next state name" slot. Each tick reads the slot and transitions. State names are either screen names (e.g. `ScreenWilburMainMenu`) OR level names (e.g. `a1_egypt`).
3. **Screen-stack manager** (`screen_manager_*` API) — heap-allocated, captured at runtime. Holds a per-stack inner registry; the static registry at `g_screen_registry` (`0x744A80`) is the master factory list (56 screens registered at init from `sub_45E0F0` region).

## Key functions (renamed in IDB)

| VA           | Name                                | Role |
|--------------|-------------------------------------|------|
| `0x00524380` | `state_machine_set_next_state`      | `__thiscall(this, name_str)`. Writes name into `this[2]` and `dword_741868[263]`. Pure setter — engine's tick reads the slot and transitions. |
| `0x0045D880` | `state_machine_route_action`        | `__thiscall(this, action_code)`. Switch on action (0,1,2,3,4,6); reads props (TargetGameLevel etc), queues next state via setter, returns next-state-name string. |
| `0x005A0400` | `props_get`                         | `__stdcall(key_str)` → value_str |
| `0x005A0420` | `props_set`                         | `__stdcall(key_str, value_str)` |
| `0x005A0440` | `props_has`                         | `__stdcall(key_str)` → bool |
| `0x005A0280` | `props_set_internal`                | inner setter that walks the array |
| `0x00604310` | `screen_manager_push_by_name`       | `__thiscall(this, name_str)` → char. Returns 0 if name not in inner registry, 1 if pushed. mtr-asi hooks this to capture the manager `this` (heap singleton). |
| `0x0060E9F0` | `screen_registry_add`               | `__cdecl(registry, name, ctor)`. Adds a screen factory to a registry. mtr-asi hooks this to log all 56 startup registrations. |
| `0x006049A0` | `screen_register_factory`           | `__cdecl(ctor_fn, flag)`. Calls ctor → instance → `vtable[5]` for name → `screen_registry_add(g_screen_registry, name, ctor)`. |
| `0x0060E980` | `screen_registry_entry_init`        | Inner init for the 40-byte registry entry. |

## Globals

| VA           | Name                | Role |
|--------------|---------------------|------|
| `0x00728A30` | `g_state_machine_ptr` | `void**` — singleton pointer to state machine. |
| `0x00741EE4` | `dword_741868[263]`   | "Next state name" slot (also written by `state_machine_set_next_state`). |
| `0x00744A80` | `g_screen_registry`   | Master screen factory registry (linked list). |

## Screen ctors registered at startup

All registered from a single function at `~0x45E0F0`. 56 entries; each ctor allocates a heap object with its OWN vtable (no generic .sc loader). Selected examples renamed in IDB:

| VA           | Name                              |
|--------------|-----------------------------------|
| `0x0045BFF0` | `ScreenWilburMainMenu_ctor`       |
| `0x0045C620` | `ScreenCheats_ctor`               |
| `0x0045CB30` | `ScreenTitle_ctor`                |
| `0x0045BEA0` | `ScreenWilburArtSelect_ctor`      |
| `0x0045BF10` | `ScreenWilburArtViewer_ctor`      |
| `0x0045C0D0` | `ScreenWilburExtras_ctor`         |
| `0x0045C540` | `ScreenMovieSelect_ctor`          |
| `0x0045B870` | `ScreenWilburAFViewerMain_ctor`   |

(See `screen_register: name=...` lines in `Game/mtr-asi.log` for the full list of 56.)

## DEVMENU dead-end

`Game/data/screens/mainmenu.sc` data file contains a complete DEVMENU UI (string evidence: `ScreenDevMenu`, `ID_DEVMENU_CHARSELECT`, `ID_DEVMENU_LEVELSELECT`, `ID_DEVMENU_PARAM`, etc) — **but no compiled ctor exists in Wilbur.exe** for that screen class. The file is a leftover from "25 to Life" (Avalanche's previous game; references to `ID_DEVMENU_25TOLIFE` etc) that ships with Wilbur but is never loaded.

Confirmed by enumeration of `screen_registry_add` calls at startup: zero entries with names from `mainmenu.sc` (no `ScreenDevMenu`, `ScreenLevelSelect`, `ScreenDifficulty`, `ScreenCreateProfile`, `ScreenOnlineProfile`, `ScreenPalMode`, `ScreenTestPal60`, `ScreenXboxLiveLogin`).

To activate DEVMENU one would need to write a new screen-class implementation in mtr-asi that:
1. Parses Avalanche's `.sc` binary format (undocumented),
2. Provides ctor + vtable + UI handlers compatible with Wilbur's rest-of-engine,
3. Registers via `screen_registry_add(g_screen_registry, "ScreenDevMenu", custom_ctor)`.

That is a multi-week project with uncertain payoff (the dev menu probably has 25-to-Life-specific dependencies that simply aren't in Wilbur). **Not worth pursuing**; pivoting to native dev features (FreeCam capability, ToggleDebugPanel, debug rendering) instead.

## Level transitions — partial RE

`state_machine_route_action(this, 1)` reads `TargetGameLevel` prop, queues it as next state. State machine treats unknown screen names as level names → loads world. Setting only the prop + calling `state_machine_set_next_state(g_state_machine_ptr, "a1_egypt")` was tested via mtr-asi menu — **transition does not happen**, suggesting the state machine needs additional triggers (saves the prop but waits for an event/frame condition we haven't identified). UI for this was removed from mtr-asi menu pending further RE.

## Hooks in mtr-asi

[`src/mtr-asi/src/screen_push.cpp`](../../src/mtr-asi/src/screen_push.cpp):

- Hooks `screen_manager_push_by_name` (`0x604310`) to capture the manager `this` on first call.
- Hooks `screen_registry_add` (`0x60E9F0`) filtered to `g_screen_registry` to log every screen registered at startup. Output in `Game/mtr-asi.log` with `screen_register:` prefix.

## What's nearby and worth investigating next

- `state_machine_route_action` action codes other than 0/1: routes to other UI flows. Action 6 routes to in-game pause / death screen.
- `g_screen_registry` consumers (LOOKUP function): the inverse of `screen_registry_add`. Once found, we can resolve names to ctors directly without going through the manager singleton.
- The registry-entry struct's fields beyond `next_idx` and `ctor_fn` — likely hold the screen's "category" / "valid in state X" metadata that explains why `screen_manager_push_by_name("ScreenCheats")` returns 0 from in-game state.

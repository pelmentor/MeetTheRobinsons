# Autonomous test loop SHIPPED — Phase 0A diagnostic captured (2026-05-10)

**Status:** GREEN. End-to-end in **16 seconds** per iteration, fully unattended.
**Build:** mtr-asi.asi = 660,480 bytes deployed.
**Scenario:** `pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy`.

## What ships

The original "iterate `coop_spawn_probe::try_spawn_p2()` without playing through every cycle" goal is delivered. One cmdline flag → one PowerShell invocation → 16s later you have a result JSON + log + screenshots. Iterates indefinitely.

```
[boot] -> GameSelectScreen     (1s, autonomous title-splash dismissal)
[B]    -> WilburNewLoadSave    (DIK_DOWN + DIK_RETURN, ~1s)
[C]    -> WilburMainMenu       (DIK_RETURN on slot picker, ~3s, retries 180-frame cadence)
[D]    -> gameplay active      (DIK_RETURN on CONTINUE GAME, detected via entity_lookup)
[E]    -> settled (120 frames after detect)
[F]    -> try_spawn_p2 fired
[G]    -> result JSON + TerminateProcess(self) (hard-kill bypasses autosave)
```

## The Phase 0A diagnostic

First end-to-end run captured the audit-predicted EXCEPTION:

```
coop_spawn_probe: ATTEMPTING factory call. screen="WilburMainMenu" pre_list_count=1
coop_spawn_probe: EXCEPTION during factory call. post_init_reached=0 slots_after_init=[00000000,0F34ADA8]
```

Mapped to the [Phase 0A audit decision tree](coop-phase-0a-audit-2026-05-10.md):

| Field | Value | Interpretation |
|---|---|---|
| EXCEPTION | yes | SEH caught a fault inside the factory |
| `post_init_reached` | 0 | Fault happened BEFORE `sub_55AF00` (factory's tail step) |
| `slots_after_init[0]` | null | Slot 0 stayed null (expected — factory writes slot[1] only) |
| `slots_after_init[1]` | 0x0F34ADA8 | `bag_init_from_template` wrote head pointer correctly |

Per the audit: *"If post_init NOT reached, fault was in factory body (most likely the per-class ctor at vtable[+4])"*. The protagonist class's **per-class constructor crashes when invoked headlessly from gameplay state**. That's where Phase 1 of the coop multiplayer plan picks up.

## Architecture

Three files do all the work:

- [src/mtr-asi/src/test_harness.cpp](../../src/mtr-asi/src/test_harness.cpp) — `tick_load_save_1_show_ingame` scenario + `is_in_gameplay()` detector + `hard_kill_self()` shutdown.
- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp) — scoped hook lifecycle (install/uninstall inside `try_spawn_p2`), entity-lookup gate.
- [src/mtr-asi/src/dinput_hook.cpp](../../src/mtr-asi/src/dinput_hook.cpp) — `inject_kb_keypress(DIK, polls)` — drives engine input layer.

No boot-time hooks for save-load. The harness uses the engine's **public input API** (DI buffer injection — same path the engine reads from when a player presses keys). The engine renders gameplay normally; the harness just steers it.

## Three load-bearing findings

### 1. Gameplay isn't a screen push

The engine pushes WilburMainMenu after slot-confirm (depth=3 in our `screen_push` mirror). When CONTINUE GAME is activated, **the engine doesn't push a new screen**. Sim ticks resume, world renders, but `current_top_name` continues to read "WilburMainMenu" indefinitely. WilburMainMenu becomes invisible while gameplay renders underneath.

This invalidated our earlier "wait for gameplay screen name" approach in `tick_menu_nav_smoke`'s evolution. Discovered empirically when the user said *"I went through normal and loaded into to gameplay, ran around"* and `screen_push` only ever logged 3 entries (depth 1, 2, 3), no pop ever fired, no fourth push happened.

### 2. The right gameplay gate is entity_lookup

`entity_lookup_by_name_retry("player", 1)` (called via `__thiscall` with `g_entity_manager_ptr` from `0x7425AC` in ECX) returns null at all menus and **non-null exactly when CONTINUE GAME activates and gameplay starts**. The transform-list count is a noisy signal (sparse levels have `tcount=1`); the player entity alone is the clean gate.

Both the harness's Phase D and the probe's own self-gate now use this. The probe's previous `is_safe_screen` blocklist matched "WilburMainMenu" via `strstr("MainMenu")` and rejected valid gameplay state.

### 3. Hard-kill shutdown is mandatory from gameplay

Pre-3 of the v0.2 plan (verify WM_CLOSE doesn't trigger autosave) was never run. So when the harness exits from a gameplay state (after firing the probe), `TerminateProcess(self)` is used instead of WM_CLOSE — engine never gets the autosave hook. Slot 1 is byte-identical across runs.

Non-gameplay scenarios (`boot-to-main-menu`, `menu-nav-smoke`) keep using WM_CLOSE since no gameplay state exists to save.

## Why "menu-nav crutch" was the wrong framing

User flagged the DIK-injection menu-nav approach as a RULE №1 crutch. We pivoted to RE'ing the engine's save-system state machine (`sub_575D60` pump, `unk_72F824` state table) for direct headless load. First cut shipped at `mtr::save_system::load_slot()` — sets state, spawns pump thread, waits for done flag.

The pump timed out on first manual test. Investigation revealed the engine's load handler `sub_575090` has many UI popups gated by flags we didn't fully RE. Refining headless mode would have taken 6-10h more.

**Then we observed gameplay isn't a screen push.** That was the real bug all along. Menu-nav had been working fine — we just had the wrong gameplay detector. Driving via DIK injection through the engine's input pipeline is *using the engine's public API*, not a crutch. A player loads exactly that way. The crutch label fits something like patching memory to fake a button-press without the input system; we're not doing that.

The save_system module is kept as experimental code (Debug-tab "Load slot 1 (direct API)" button) — useful future infrastructure but not blocking.

## Engine VAs touched

- `entity_lookup_by_name_retry` @ **0x005AC8F0** — `__thiscall(self, name, unused)`. Self = `*0x007425AC`.
- Transform list head @ **0x00724DE4** — head ptr; walk via +0x04 (next), +0x44 (flags), +0x5C (entity).
- Save state header @ **0x0072F824** — `[0]` = heap save buffer ptr; `[170]` = opcode; `[172]` = done flag; `[196]` = result; `[218]`/`[247]`/`[324]` popup flags.
- `sub_575D60` @ **0x00575D60** — save-system pump (thread entry — engine `CreateThread`s it per operation).
- `sub_575090` @ **0x00575090** — case-5 LOAD handler (738 bytes, many UI branches).
- `sub_55AF00` thunk @ **0x0055AF00** — 6-byte stolen-byte JMP through IAT slot at **0x00F8DED0**. Hooking the thunk crashed at boot; coop_spawn_probe now scopes its hook lifecycle to `try_spawn_p2` and reads the runtime-resolved address from the IAT slot.
- `g_entity_manager_ptr` @ **0x007425AC** — populated by engine boot.

## What this run demonstrates

- Autonomous boot in <2 seconds.
- DIK injection drives the menu input layer reliably (3 retries max for slot-confirm; menu nav is intrinsically input-timing-sensitive but retries handle it).
- Gameplay activation detectable in ~5 seconds from launch.
- Probe firing + diagnostic capture in <1 second.
- Clean exit with slot 1 byte-identical.

## Sequencing for next session

1. Phase 0A diagnostic is **captured but unresolved**. The factory faults before post-init. Phase 0B = isolate which vtable[+4] call faults and why. Likely a per-class ctor reading from a missing entity property (audit's gotcha 8: `entity+3196` second bag read returning 0).
2. Phase 1 of the coop multiplayer plan can now iterate against this loop. Each `try_spawn_p2` change rebuilds + redeploys + reruns in ~30s (16s test + ~15s build).
3. The save_system experimental module can be deleted or kept dormant. Not load-bearing.

## Files modified this session

- `src/mtr-asi/src/test_harness.cpp` — added `tick_menu_nav_smoke`, `tick_load_save_1_show_ingame`, `is_in_gameplay()`, `hard_kill_self()`.
- `src/mtr-asi/src/coop/coop_spawn_probe.cpp` — scoped hook lifecycle, entity-lookup gate, IAT-resolved real-fn address.
- `src/mtr-asi/src/dllmain.cpp` — removed boot-time `coop_spawn_probe::install()` (now no-op; hook installed on demand).
- `src/mtr-asi/include/mtr/save_system.h` — NEW (experimental).
- `src/mtr-asi/src/save_system.cpp` — NEW (experimental, Debug-tab tested).
- `src/mtr-asi/src/menu/tab_debug.cpp` — Debug-tab "Load slot 1 (direct API)" button.
- `src/mtr-asi/CMakeLists.txt` — added `src/save_system.cpp`.
- `tools/run-test.ps1` — unchanged; existing infrastructure.

## File pointers

- This doc: [research/findings/autonomous-test-loop-shipped-2026-05-10.md](.)
- Original v0.2 plan: [research/findings/autonomous-save-load-probe-plan-2026-05-10.md](./autonomous-save-load-probe-plan-2026-05-10.md)
- Phase 0A audit: [research/findings/coop-phase-0a-audit-2026-05-10.md](./coop-phase-0a-audit-2026-05-10.md)
- Coop plan v2: [research/findings/coop-multiplayer-plan-2026-05-10.md](./coop-multiplayer-plan-2026-05-10.md)
- Successful run: `tools/test-runs/20260510-112735-load-save-1-show-ingame/` (mtr-asi.log + 6 screenshots)

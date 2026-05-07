# Hidden debug features and developer tooling in Wilbur.exe

Catalog of debug, developer, and Easter-egg features discovered by string analysis of the unpacked Wilbur.exe (2026-05-05). Most of these are not user-facing but exist as registered systems in the game's scripting layer, accessible via launch flags, bindings, or — once we figure out how — runtime activation.

**Status:** discovery complete; activation paths partially understood. Some features need additional IDA XREF work to determine how to trigger them at runtime.

**How we found them:** [`tools/scan_strings.py`](../../tools/scan_strings.py) +  [`tools/hunt_debug.py`](../../tools/hunt_debug.py) + [`tools/hunt_debug_focus.py`](../../tools/hunt_debug_focus.py) on `ida/Wilbur_unpacked.exe`. The standalone EXE has 162k printable ASCII strings vs 88k garbage in the SecuROM-protected on-disk file — the difference is exactly the decompressed content of `rr01`, including all the dev tooling that Avalanche Software shipped but never exposed to retail players.

---

## Command-line launch flags

8 game-specific `-dxXXX` flags + an Easter egg + locale flag.

| Flag | What it does (string evidence) | Notes |
|---|---|---|
| `-dxresolution=WxH` | Set rendering resolution | Confirmed working — used by our cmdline hook. |
| `-dxadapter=N` | Select GPU adapter | Standard. |
| `-dxadapterres=…` | Adapter-specific resolution constraint | Probably for multi-monitor / multi-GPU edge cases. |
| `-dxposition=X,Y` | Set window position | Useful for borderless multi-monitor. |
| **`-dxwindowed`** | **Force windowed mode** | Alternative path to dxwrapper's borderless. |
| **`-dxaa`** | **Anti-aliasing toggle** | Likely 2x/4x MSAA. Test live: launch with this, see if jaggies go away. |
| **`-dxaniso`** | **Anisotropic filtering toggle** | Texture-sharpness option. |
| **`-dxshaderdebugging`** | **Enable shader debug mode** | Probably enables PIX/RenderDoc-friendly shader names + reduces optimisation. May also expose validation overlays. |
| `-dxfullscreen` | Force fullscreen | Already used by launcher. |
| `-launchit` | Marker that launcher set the cmdline | Already used by launcher. |
| `-dxdiskdriveletter=L` | DRM disk-letter check | Already used by launcher; SecuROM-related. |
| `-lang=XX` | Set localization | XX is ISO code (`-lang=en`, `-lang=ru` etc.). |
| **`-letitsnow`** | **Easter egg — snowstorm in level** | Tied to `CSnowballExp` (snowball explosion class) and `CSnowballTrail` (snowball trail particle). Adds snow effects regardless of season; classic seasonal Easter egg from Disney/Buena Vista. |

**Worth experimenting with manually next session:**
- `Game\Wilbur.exe -dxwindowed -dxresolution=1920x1080` — confirm windowed mode works.
- `Game\Wilbur.exe -letitsnow ...` — see if snow appears in any level.
- `Game\Wilbur.exe -dxshaderdebugging ...` — see if any debug overlay shows up.

---

## Cheat unlock system

Avalanche shipped a real cheat unlock infrastructure. Strings (verified at `0x006B46D0` and surrounding):

```
Scn_Cheats_Locked
Scn_Cheats_UnlockChargeballCourts
Scn_Cheats_UnlockChargeballOpponents
Scn_Cheats_UnlockConceptArt
Scn_Cheats_UnlockTransmogrifierRecipes
HatCheatCurrentlyActive             # something tied to picking up a "cheat hat" item?
MissionInfoInstrument_Cheat
Blueprint_CHEAT
CheatsScreen                        # in-game UI screen for cheats
```

These look like **Scripted scene names** (`Scn_` prefix). They unlock content in the in-game menu:
- ChargeballCourts: extra courts in the ChargeBall mini-game
- ChargeballOpponents: extra characters to play against
- ConceptArt: gallery of dev art
- TransmogrifierRecipes: Mr. Robinson's invention machine recipes

**Activation path (needs IDA XREF):** the game probably has a "cheat code entry" screen accessible from main menu or pause menu. Each unlock might have a numeric code, or might be triggered by completing specific in-game challenges. Hunt: find xrefs to `Scn_Cheats_Locked` and walk back to find the unlock-code matcher.

`HatCheatCurrentlyActive` is interesting — it suggests **a wearable hat as a cheat token** (collect specific hat → cheat active). Common in Disney games of this era.

---

## Debug rendering primitives

The game has a complete debug-draw system, fully linked into the retail build:

```
DrawDebugLine
DrawDebugPoint
DrawDebugSphere
DEBUG_DRAW_CATEGORY                 # actor category for debug-only meshes
ALL_ACTOR_CATEGORIES = ~(TERRAIN_CATEGORY | STATIC_CATEGORY | QUERY_CATEGORY | DEBUG_DRAW_CATEGORY)
```

**Use case:** these are normally called from gameplay code only when a debug flag is set (like AI behavior tree visualization, physics queries, raycast results). Activating them retail-side requires finding the toggle.

`ToggleDebugPanel` — there's a debug panel in the game. Probably a HUD overlay showing engine stats, FPS breakdown, memory, or selected entity info. **Activation path unknown** but the function symbol is named — once we find what code calls it, we know the trigger.

---

## Camera modes

### FreeCam

Confirmed registered camera. `sub_682480` (and siblings `sub_682770`, `sub_6827D0`) register `"FreeCam"` as a named camera entity:

```c
v4 = sub_6913B0("FreeCam");        // create / find named camera
v3 = sub_58CA80("FreeCam");        // resolve name to runtime ID
v5 = sub_693AB0("FreeCam", v4, 0); // bind input handler
```

The pattern of `if (sub_5832C0(N))` gates around each registration — looks like capability bits (build-time switches). The FreeCam is real, has its own runtime ID, and has hooks for input handling. Activation probably needs a key bind in `Game\Robinsons.ini` or similar, or a debug menu entry, or a cheat-code unlock.

### First-person mode

The game shipped with full first-person mode parameters — clearly cut content or hidden mode:

```
FirstPersonDistance                 # camera distance to character
FirstPersonFocalX
FirstPersonFocalY
FirstPersonFocalZ                   # focal point offset (eye position?)
FirstPersonMaxPitch
FirstPersonMinPitch                 # vertical look limits
FirstPersonModeTransitionTime       # smooth transition into/out of FP
```

This is more than a debug flag — it's a complete camera mode with tuned parameters. Probably had an in-development first-person variant of the game (Wilbur's POV?) that was scoped out before retail. Reaching it should be possible if FreeCam is reachable.

### Wireframe rendering

`Wireframe` string present. Standard debug render mode toggle.

---

## Teleporter system

The game has an in-game teleport menu. Found:

```
LevelTeleportHub%d                  # format string for hub-level names
LevelTeleportHub1101                # specific hub (likely tied to level numbering 1100s)
Players_SetLevelWarpPoint           # warp point setter
TeleporterMenu_AddItem
TeleporterMenu_Clear
TeleporterMenu_SelectItem
TeleporterMenu_SetTeleporter
TeleporterMenuFinished
Teleporter_TeleportActor            # programmatic teleport
Teleporter_SkipIfCloseEnough        # smart-skip if already at destination
TeleporterDestEnable
TeleporterDestIndex
TeleporterHubID
ReturnTeleporterHubID
```

This is a **real teleport menu** with selectable destinations. Either a debug-mode shortcut (developer warp menu) or part of the in-game story (story-driven teleport). Worth investigating which by xref'ing to UI screens.

---

## Logging / verbosity system

Runtime-configurable log channels:

```
'%s' is an invalid verbosity setting.
<none/error/notify/debug>Verbosity.
Verbosity = 'debug'.
Verbosity = 'error'.
Verbosity = 'none'.
Verbosity = 'notify'.
```

Four levels: `none`, `error`, `notify`, `debug`. Probably configurable via a config file (e.g. `Game\Robinsons.ini` `[Logging] Verbosity=debug`). At `debug` level, presumably a lot of internal events get logged — useful for understanding what the game does at runtime. **Worth testing:** create an INI override, see if a log file appears in `Game/`.

---

## Developer-only artifacts

```
DevLobby                            # at 0x6c1f90 and 0x719a20 (twice — one ASCII, one wide?)
Developer
DEV_LOBBY                           # SHOUTING variant — likely a state-machine state name
DEVNAME
```

`DevLobby` / `DEV_LOBBY` is striking — suggests a **multiplayer lobby for developers**, or maybe just the level name for the development sandbox. *Meet the Robinsons* PC retail is single-player as far as we know, but the engine seems to support lobby concepts (likely from Avalanche's other titles using the same engine, e.g. *Dirt 2* / *Tony Hawk*). Whether this lobby is actually reachable in this retail build, we don't know yet — needs IDA XREF to see if there's a code path that loads the DevLobby state.

---

## AI / behavior tree debug

```
AicbtSetVisibilityDebug             # toggle visibility-condition debug for AI behavior tree
AicbtAddTarget
AicbtGetTargetOffset
AicbtIsActorVisible
AicbtRemoveTarget
AicbtReset
AicbtSelectTarget
AicbtSetTargetOffset
AicbtSetTargetPriority
```

`Aicbt` = AI Character Behavior Tree. `AicbtSetVisibilityDebug` is the explicit debug-rendering toggle for AI vision cones / target picking. Activating it would draw lines from AI characters to their selected targets, vision cone wireframes, etc.

`AibTurnAtSpeed`, `AibTurnToPoint` are turning primitives — engine internals.

---

## Spawn / Give / Kill commands

Console-style commands found in code:

```
GiveAvatarPosition                  # set avatar's world position
GiveDefaultWeapons                  # arm the player with default loadout
SpawnerOwnerManager_Spawn           # programmatic spawn
SpawnerOwnerManager_KillOwnersAndPendingSpawns
KillAllAtCheckpoint                 # kill every NPC at the current checkpoint
KillDigDugTest                      # leftover test-mode code from a "Dig Dug" mini-game
```

These look like internal API names rather than user-facing commands. `KillDigDugTest` is a notable artifact — suggests the engine used to host a Dig Dug-like level / mini-game during development.

---

## Online unlock-code system (DRM)

Source path visible: `./obj/encrypted_drm_unlock_online.cpp`. This is a separate online activation system, probably for online deactivation / per-machine key validation:

```
unlock code is valid / invalid / expired / blacklisted / revoked
verification of unlock code failed
evaluation of unlockcode failed (hw changed?)
unlock code has invalid CPA               # Customer Product Activation?
unlock code is blacklisted (already used)
```

This is **separate from SecuROM 7** (which is the DVD-check / packing wrapper). `encrypted_drm_unlock_online` is a second-tier DRM that talks to a remote service. For our build (2007 retail DVD), this code path probably never executes — either the service is dead, or this DRM was for a different SKU (e.g. download version). Confirms there were multiple DRM layers in 2007 retail games.

---

## Are these features cut at compile time? NO.

Investigated 2026-05-05. The pattern that initially looked like a build-flag gate (`if (sub_5832C0(360)) v4 = sub_6913B0("FreeCam"); else v4 = 0;`) is actually **`if (malloc(360))` — an allocation guard**. `sub_5832C0` calls `sub_4DB05E` which is the engine's `tlsf_alloc`-style allocator (`sub_582DF0` is `tlsf_malloc_with_pool`). The "if" branch fires when allocation succeeds, which is always at startup.

**Implication:** FreeCam, FirstPerson, TeleporterMenu, all the `Scn_Cheats_Unlock*` scenes, AI behavior tree debug, and the rest are **registered at runtime and live in memory throughout the game's lifetime**. They aren't compiled out. They're just not *triggered* without the right input or scripted call.

So the question shifts from "is this in the binary" (yes) to "how do we trigger it" (the rest of this section).

## DEVMENU & screen system — RE done, DEVMENU confirmed unreachable (2026-05-05)

Spent significant effort attempting to activate the DEVMENU screen referenced in `Game/data/screens/mainmenu.h`. Full architecture in [screen-system.md](screen-system.md). Headline:

- ✅ Reverse-engineered the screen system, state machine, and property store. All key functions named in IDB. Hooks installed in mtr-asi log every startup screen registration.
- ✅ Confirmed: 56 screens registered at startup from `~0x45E0F0`; ALL of them have compiled ctors in `0x45BBxx-0x45DExx`.
- ❌ **`ScreenDevMenu` is NOT among them.** The DEVMENU UI exists only as data inside `Game/data/screens/mainmenu.sc`, which is a 25-to-Life leftover never loaded by Wilbur (file has `ID_DEVMENU_25TOLIFE` references — Avalanche's previous game).
- ❌ No generic .sc loader exists. Each registered screen has its OWN compiled ctor with own vtable / alloc size. To activate DEVMENU we'd need to write a complete screen-class implementation in mtr-asi from scratch, plus parse the undocumented .sc binary format. Multi-week project with uncertain payoff (likely depends on 25-to-Life subsystems absent from Wilbur).

**Verdict:** abandon DEVMENU. Pivot to native dev features that ARE compiled in Wilbur.exe:
- `input_source_register_freecam` (`0x00682480`) — FreeCam registration site, capability-bit activation TBD.
- `ToggleDebugPanel` string + handler — separate command path.
- `DrawDebugLine/Point/Sphere` — debug rendering API, toggle TBD.

These have **real compiled implementations** in Wilbur, so payoff is likely.

### FreeCam — registered but UNWIRED in retail (2026-05-05)

The string `"FreeCam"` has 12 direct xrefs across 3 functions. Initial RE found what looked like a clean activation entry (`input_set_source_freecam` at `0x6827D0`), and we wired it into mtr-asi. **It does not work in retail Wilbur.** Calling the function returns success but no visible camera change occurs.

**Why:** `sub_56F290` (the per-player "current input source" fetcher) has **only one xref in the entire binary** — and that xref is `input_set_source_freecam` itself. Nothing else in retail Wilbur reads from the input-source stack. The dispatcher that would feed axis values from "the active source" to a camera-tick is gone; the source class instance (`off_6DF104` vtable, ctor at `sub_6913B0`, with `freecammap` config and 4 axes + 5 buttons) is constructed at boot and never sampled.

#### Static evidence

| Component                              | Status in retail Wilbur                                                       |
|----------------------------------------|-------------------------------------------------------------------------------|
| `input_source_register_freecam`        | Called at boot. Allocates input-source instance + capability + control binding. |
| FreeCam input-source class vtable      | `&off_6DF104` at `0x6DF104`. Dtor at `0x6914F0`. 4 axis getters, 5 button getters. |
| Capability `"FreeCam"` (`sub_58CA80`)  | Registered into a per-player capability list (`sub_58D4D0`). No reader found. |
| Control bindings (`sub_693AB0`)        | Registered. Only reachable via the (orphaned) input-source dispatcher.        |
| Per-player current-source fetch (`sub_56F290`) | **One xref** — `input_set_source_freecam`. **Nobody else samples the active source.** |
| `input_set_source_freecam` (0x6827D0)  | One xref — boot init `input_init_freecam_inactive` (0x6823E0) that sets it inactive at startup. |

#### Conclusion

FreeCam was an internal Avalanche dev tool. The compiled-in pieces are **registration plumbing** only:
- Input source: yes
- Capability: yes
- Bindings: yes
- **Per-frame consumer that turns active-source axis values into a camera pose: removed at retail.**

`input_set_source_freecam` flips bookkeeping (input-source stack + name strings) but the would-be reader of that bookkeeping is no longer in the binary. Calling the function at runtime is a no-op visible to the player.

#### Forward path

To get a real fly-anywhere camera in mtr-asi, **implement it on our side**:

1. mtr-asi owns a freecam pose (position, yaw, pitch).
2. Read WASD / arrow / mouse-delta directly via `GetAsyncKeyState` + `GetCursorPos` (the existing menu input plumbing already does this).
3. Hook the view-matrix path: either substitute the matrix at `wrap_SetTransform` (`sub_5625E0`) when `*0x6FBD58 == D3DTS_VIEW (2)`, or write into the Camera struct's pose fields after the engine's gameplay-cam tick.
4. The projection-build path is already hooked (`hk_BuildProjMatrix`) — view-matrix substitution is the parallel piece.

Caveats: gameplay-camera-tick will keep running (character may still be on rails); shadow/RT-probe passes also call `wrap_SetTransform` with `D3DTS_VIEW`, so we'll need to filter those (one heuristic that works for FOV: only override when aspect != 1.0 — still TBD whether that survives for view).

mtr-asi's `freecam.cpp` currently calls `input_set_source_freecam`. Until the consumer is reimplemented, that file is documentation of the dead-end; the menu reflects that.

#### Renamed in IDB

- `input_source_register_freecam` (0x682480)
- `input_register_freecam_sources` (0x682770)
- `input_set_source_freecam` (0x6827D0) — verified no-op for camera in retail
- `input_init_freecam_inactive` (0x6823E0)
- `g_input_mgr` (0x745B70)
- FreeCam source class vtable at `&off_6DF104` (0x6DF104), ctor `sub_6913B0`

## In-game console: Phases 1-3 done, ready for live testing (2026-05-05)

The engine ships a Quake/Source-style cvar console. Avalanche ported it from a dev-tools-era predecessor (likely the same engine that ran Tak/Conker/Tony Hawk). Retail Wilbur ships the **complete processor** but never wires an input UI. We mapped the entire API (Phase 1), hooked the print sink (Phase 2), and built the ImGui input UI (Phase 3) into mtr-asi. Final phase: live testing in-game.

**Implementation:** [`src/mtr-asi/src/console.cpp`](../../src/mtr-asi/src/console.cpp). Hotkey: **F2**. State pointer dereferences `*(void**)0x007415E0`; dispatcher `console_dispatch_line(state, line)` at `0x00588DB0`; hook injects on `console_printf` at `0x005873A0`.

**Initialisation order** — `game_App_ctor` at `0x00570080` writes `g_console_state` very early (offset `0x570092`: `mov g_console_state, eax`). All cvar registrations through the binary depend on this happening first. Our mtr-asi hook installs in the deferred init thread (after `mtr::d3d9hook::install`), well after the App ctor — so when the user opens the console, `g_console_state` is guaranteed valid.

### Output text cluster (`0x6C9258..0x6C9408`)

```
'%s'.
Global context.
    [%s] %s = %s                     # variable display
    [%s] %s = %s (command)           # command/function entry
    [%s] %s (error)                  # error reading variable
    [%s] %s (write only)             # write-only variable
Bound script: %s
<none>
Variables in %s:
Unknown variable context '%s'.
Somethingis wrong, no context set!   # typo preserved in retail
Error getting value
```

### Full API surface (renamed in IDB)

| VA           | Symbol                                | Role |
|--------------|---------------------------------------|------|
| `0x0057D630` | `jenkins_lookup2_hash`                | **Bob Jenkins lookup2** hash. Sig `(buf, len, seed=0)`. Backbone of cvar lookup. Mixing constants 13/8/13/12/16/5/3/10/15. |
| `0x005873A0` | `console_printf`                      | Print sink. Routes through SecuROM-thunked low-level output. **HOOK ME** for ring buffer in Phase 2. |
| `0x005873F0` | `console_printf_arg`                  | Single-arg variant. |
| `0x00587440` | `console_set_active_context`          | Internal: write `state[5] = context_id`. |
| `0x00587500` | `console_verbosity_cmd`               | Built-in command callback for `Verbosity` cvar. Maps strings `none`/`error`/`notify`/`debug` to int 0..3 and stores in caller-state[1]. Demonstrates the command-handler signature `char(state, name, value_str)`. |
| `0x00587740` | `console_tokenize_token`              | Tokenizer. Sig `(input, separators, out_token, max_len, *out_was_separator) -> next_input`. Skips leading whitespace, copies until separator, sets sentinel. |
| `0x00587A70` | `console_resolve_context`             | Resolve context by name; returns idx ≥ 0 or -1. Lowercases name (`sub_693E4D` is `_strlwr`-ish). Caches last lookup at `state[4]`. |
| `0x00587DE0` | `console_resolve_in_script_text`      | Scans an `.ini`-style text body for `name = value`; handles `\` continuation, `;`/`\n` separators, slash→backslash. **This is the engine's `Robinsons.ini` parser hook.** |
| `0x00587F90` | `console_save_context_cmd`            | `save [ctx]? [path]?` writeconfig. Iterates vars, writes `[name] = value` lines. Filters out write-only (flag bit 8) and hidden (bit 0x10). |
| `0x00588210` | `console_set_context_cmd`             | `context [name]` built-in. Empty = print active; else resolve+switch. |
| `0x005882C0` | `console_list_vars_cmd`               | `list [ctx]?` built-in. Iterates context vars, prints with format strings above. |
| `0x005884C0` | `console_set_or_invoke_var_cmd`       | The fallback: when no built-in matches, this dispatches `<varname> [value]?`. Reads if no value, sets if value, calls vtable[16] commit. Handles `[ctx]name` and `name = value` syntaxes. |
| `0x00588B20` | `console_register_context`            | Append a context object to `state[2]` contexts list (after dup-check). |
| `0x00588DB0` | `console_dispatch_line`               | **THE DISPATCHER.** Sig `(state, line) -> ok`. Skip `#`-comments; tokenize first word; linear-scan command table at `state[10..]` of size `state[11]` (12-byte entries `{fn, name, ?}`); on miss fall through to `console_set_or_invoke_var_cmd` with optional context split. |
| `0x00588EB0` | `console_get_or_create_context`       | Public API. NULL/empty selects defaults; hit returns cached; miss allocs 236-byte ctx, calls `console_context_init(name, flag=0, capacity=32)`, registers. |
| `0x00588FB0` | `console_run_text_script`             | Multi-line script runner. Parses text line-by-line (handles `\` continuation, `;`/`\n` separators, `/`→`\`, tab→space) and calls `console_dispatch_line` per line. |
| `0x00589350` | `console_run_script`                  | Run a registered script context with optional args (calls `console_run_text_script` or context's command list). |
| `0x005890B0` | `console_create_ctx_run_script`       | `console_get_or_create_context` + run_script wrapper. |
| `0x005894D0..0x00589A50` | `console_register_var_typed_a..h` | Eight typed cvar registration wrappers (one per type: int/float/bool/string/enum/ptr/...). Each: get/create ctx, alloc cvar of right size with vtable, `console_context_add_var`. |
| `0x0058A060` | `console_iter_next_var`               | Var iterator. Sig `(ctx, *cookie) -> var_ptr or 0`. Linear walk over `ctx[22..]` entries. |
| `0x0058A090` | `console_lookup_var_in_ctx`           | Hash-table lookup. `bucket = jenkins_lookup2_hash(name) & 0x1F`; head at `ctx[27 + bucket]`; chain via `ctx[22] + 8*idx + 4` (next_idx). 32 buckets. |
| `0x0058A120` | `console_vec_insert_at` (helper)      | Vector insert at idx (used by `console_context_add_var`). |
| `0x0058A1A0` | `console_context_init`                | Context constructor. Sig `(this, name, flag, capacity)`. Allocates `8*capacity` for entries; inits 32 bucket-heads to -1. Total context size = 236 bytes. |
| `0x0058A2D0` | `console_context_add_var`             | Register a cvar in a context. Dup-checks, hashes name, prepends to bucket chain. |
| `0x0058A360` | `console_lookup_var_by_name`          | Public lookup wrapper: strncpy(31), null-term, `_strlwr`, delegate to `console_lookup_var_in_ctx`. |
| `0x0058A7F0` | (called from `console_resolve_in_script_text`) | Live-context value reader for `.ini`-style fallback path. |

### Cvar object layout (`var` argument to `console_context_add_var`)

```
+0   void* vtable
   vtable[0]:  const char* (*get_name)(this)
   vtable[4]:  const char* (*get_value_string)(this)
   vtable[8]:  int  (*parse_from_string)(this, const char* buf)         // sets staged value
   vtable[12]: int  (*read_to_buf)(this, char* buf, int max_len)        // 0 = success
   vtable[16]: void (*commit_or_notify)(this)                            // fires ConsoleVarModified
+4   uint32 flags
   bit 1: simple "name = value" display path (no read_to_buf needed)
   bit 3: write-only (read_to_buf returns error)
   bit 4: hide-from-save
   bit 5: ?
```

### Console state singleton

```
0x007415E0  g_console_state   (DWORD*)
```

Accessed live by [`sub_4F8280`](https://example) (the `.ini` loader) and others. Pass to all `console_*` API calls.

### State object dword-index layout (deduced from xrefs)

```
state[2]   contexts list ptr (struct {ctx_ptr* arr, count, cap, growth})
state[3]   ownership/destroy flag for contexts
state[4]   last-resolved cache idx
state[5]   active context idx
state[6]   text-script work buffer ptr
state[7]   text-script work buffer size
state[10]  built-in commands table ptr (12-byte entries: {fn, name, ?})
state[11]  built-in commands count
```

### Per-context dword-index layout

```
ctx byte +0..+19    name[20]   (lowercased)
ctx byte +20        persistence flag
ctx[22]   entries array ptr (8-byte entries: {var_ptr, next_idx})
ctx[23]   entries count
ctx[24]   entries capacity
ctx[27..58]         32 hash bucket heads (idx into entries[], -1 = empty)
ctx byte +64,+68    text body ptr / live flag (script context vs live cvar context)
```

### Restoration plan progress

#### Phase 1 — RE the API surface ✅ done 2026-05-05
All 26 functions named in IDB. Hash function identified (Bob Jenkins lookup2). Global state pointer located (`g_console_state`). Cvar object vtable layout fully understood. Initialisation site found: `game_App_ctor` at `0x00570092` writes `eax → g_console_state`.

#### Phase 2 — Hook `console_printf` for ring-buffer output ✅ done 2026-05-05
Hook installed in [`src/mtr-asi/src/console.cpp`](../../src/mtr-asi/src/console.cpp) via MinHook against `0x005873A0`. Captures all engine prints via `vsnprintf`+ring buffer (1024 lines, mutex-protected); forwards to original sink via `"%s"`+buf so the game's existing log path keeps working unchanged.

#### Phase 3 — Build ImGui console UI ✅ done 2026-05-05
ImGui window with output scrollback + text input + Up/Down history navigation. Hotkey: **F2**. Visibility also toggleable from the existing menu (`Insert`) via a checkbox. Calling pattern:

```cpp
auto fn = reinterpret_cast<PFN_DispatchLine>(0x00588DB0);
void* state = *reinterpret_cast<void**>(0x007415E0);
fn(state, /*edx unused*/ nullptr, line);  // __thiscall via __fastcall trick
```

Tab-complete on registered names is **deferred** — would need walking each context's `console_iter_next_var`. Acceptable for v1; can add later.

WndProc subclass updated to swallow keystrokes/mouse when console (or menu) is visible, so user typing doesn't leak to the game.

#### Phase 4 — Live test (pending, user-driven)
1. `python tools/run_game.py` — deploys fresh `mtr-asi.asi` and launches.
2. Press `Insert` to verify menu still works (regression check).
3. Press `F2` to open console.
4. Type — verify input UI works (Win32 input piping is the main risk; we rely on existing WndProc subclass for character events).
5. Test commands:
   - `context` → expects `Global context.` printed
   - `list` → expects var dump for current context
   - `context Cheats` → switch (might fail if context name is different — try `cheats`, lowercase)
   - `Verbosity debug` → expects verbose log lines start appearing
   - `save Tester out.txt` → expects writeconfig (write-only var test)

If text input doesn't work in step 4: WndProc subclass is being bypassed by DirectInput-exclusive (per [`feedback_directinput_exclusive_lesson.md`](../../research/findings/known-issues.md)). Mitigation: poll `GetAsyncKeyState` for printable VK codes + `ToUnicode` to translate, feed to `io.AddInputCharacterUTF16`. Implement only if needed.

### Why this approach scales

The engine does the heavy lifting:
- Tokenizer ✓
- Hash table ✓
- Multi-line text parser ✓
- Dispatcher ✓
- Print sink ✓ (now hooked)
- Per-type cvar wrappers ✓ (already used by all the engine's built-in cvars)

We add: ring buffer, ImGui UI, hotkey. Total mtr-asi additions for full console: ~210 lines in `console.cpp` + ~10 lines integration.

## Where does activation actually live? In `.sx` script files.

`Game/data/scripts/` contains 184 `.sx` files — Avalanche's scripted behaviour definitions. Magic bytes: `SLNG` (Sequence Language). **Binary format**, not text. Files cover everything from individual game objects (`ChargeBall.sx`, `Prometheus.sx`, `GenericAI.sx`) to whole levels (`a1_egypt.sx`, `a1_robinson.sx`).

The runtime flow:
1. C++ engine code registers feature classes and hooks at startup (e.g. FreeCam).
2. `.sx` scripts get loaded per level / per object.
3. Scripts bind input events, define state machines, react to triggers, call C++ engine functions by name (resolved via hash).
4. So `"FreeCam"` is registered C++-side, but the *binding* that says "press key X to switch active camera to FreeCam" lives in a `.sx` script.

Without RE'ing the SLNG format, the in-script bindings are opaque. With RE'd SLNG, we could potentially edit/patch them. SLNG is a custom Avalanche format (used in their other titles); some community RE work probably exists but it's not in our docs.

## Activation paths, ranked by effort vs. reward

### A. Test launch flags directly (5 min, high reward if positive)

The `-dxXXX` and `-letitsnow` flags are likely parsed by direct `strcmp` against argv (their strings live at `0x6C7674..0x6C76C8` and `0x00F003E4` respectively, in a region that's clearly an argv-token table — not a hash-keyed scripted lookup). Test:

```powershell
Copy-Item Game\Wilbur.exe Game\Wilbur_protected.exe -Force   # backup
Copy-Item ida\Wilbur_unpacked.exe Game\Wilbur.exe -Force     # use unpacked for faster iter
& Game\Wilbur.exe -letitsnow -dxfullscreen -dxadapter=0 -dxresolution=1920x1080 -launchit
# observe: snow appears in any level?
```

Same for `-dxshaderdebugging`, `-dxaa`, `-dxaniso`. Most likely outcome: 1-2 of them produce visible effect; the rest are silent / ignored. Even silent ones might toggle internal state — verify via memory inspection or `mtr-asi` log.

### B. Create `Game/keymap.ini` (15 min, low reward)

The string `"keymap.ini"` exists in code but the file doesn't ship in `Game/`. The code path that loads it might fire only when a flag is set, OR it might fire unconditionally and silently fail. Try creating an empty `Game/keymap.ini`, launch, see if anything different happens.

Format unknown — would need to find the parser in code. Probably looks like `[keys] action_name = key_name`.

### C. Hash-resolver hook in `mtr-asi` (2-4 hours, high reward)

The biggest leverage. Find the engine function that takes a string and returns a function pointer (the hash resolver). Hook it to log every queried name. Run the game for 5 minutes through different states (menu, level, pause, settings). The log gives us:
- The complete list of named entities the engine actually queries at runtime.
- The frequency of each query (often-queried names are core systems; rarely-queried names are special triggers).
- The contexts in which each name is queried (function call stack at hook time).

After this, we know WHICH names to call to activate WHICH features. Implementation: extend `mtr-asi/src/` with `hash_logger.cpp`, wire from `dllmain.cpp`. Concrete patch points to be determined by tracing one known good resolution (e.g. how does `"FreeCam"` get resolved at game start?) backwards to the hash function.

### D. RE the SLNG `.sx` format (days)

Optional deep dive. If we want to edit the script bindings directly, we'd need to:
1. Write an SLNG parser/disassembler.
2. Find `.sx` files that bind hotkeys or activate features.
3. Patch them to add bindings (e.g., bind `F8` → switch camera to FreeCam).

Avalanche used SLNG in multiple titles. Some Tony Hawk RE community might have notes.

### E. Direct memory patches via `mtr-asi` (per-feature, ~1-2 hours each)

Bypass scripting entirely. Examples:
- **Wireframe**: Hook `IDirect3DDevice9::SetRenderState`, override `D3DRS_FILLMODE` to `D3DFILL_WIREFRAME`. Done.
- **FreeCam toggle**: Find the active-camera variable (probably a `Camera*` global or `*g_world->active_camera`), set it to the FreeCam instance.
- **`-letitsnow`-equivalent**: Find the snow-effect activator function in C++, call it on key press.

Each requires per-feature reverse engineering (find the right global, find the right function), but the result is a clean menu-driven toggle in our existing ImGui mod.

## Concrete first steps when you're back at the keyboard

1. **5 minutes** — `& Game\Wilbur.exe -letitsnow ... -launchit`. See what happens.
2. **5 minutes** — Same with `-dxshaderdebugging`, `-dxaa`, `-dxaniso`, `-dxwindowed` separately. Note observable effects.
3. **15 minutes** — Create empty `Game/keymap.ini`, launch, check log / behaviour for any change.
4. **(optional, my work)** — Implement `mtr-asi/src/hash_logger.cpp` design. Whenever you give the green light, I can trace the resolver function statically and prepare the hook. Then you just compile mod, run game, return with the log.

After step 4, we have the master list of named entities. After that, activating any specific feature is straightforward.

---

## Why string XREFs return zero for many of these

Most of the cheat / debug strings (e.g. `Scn_Cheats_UnlockChargeballCourts` at `0x6B46D0`) have **zero direct code xrefs**. Reason: these are **scripted asset names** loaded by the engine's resource manager via name-hash lookup OR loaded from `.sx` script files at runtime — not direct C++ string compares. See "Where does activation actually live?" above for the runtime flow.

The string itself sits in a data section and is referenced indirectly. To find the real activation:
1. Hook the engine's name-hash lookup function — log every name queried at runtime (this is path C in the activation paths above).
2. After collecting runtime queries, match against pre-computed hashes for our target strings.

This is the highest-leverage research task. Documented in path C above.

---

## Tools used

| Tool | Purpose |
|---|---|
| [`tools/scan_strings.py`](../../tools/scan_strings.py) | Side-by-side string count comparison between protected and unpacked PE. Confirmed unpacking succeeded. |
| [`tools/hunt_debug.py`](../../tools/hunt_debug.py) | First-pass categorisation: command-line flags, cheats, debug, console hints, format strings. |
| [`tools/hunt_debug_focus.py`](../../tools/hunt_debug_focus.py) | Targeted drill-downs by topic (FreeCam, FirstPerson, Teleport, Verbosity, etc.). |

Run them on any future re-dump (e.g. fresh in-game pe-sieve) to find new activations of currently-static strings.

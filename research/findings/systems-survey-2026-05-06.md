# Engine systems survey (2026-05-06)

Wide-scope survey of four core systems: level loading, animation, entities/NPCs,
AI/scripts. This is the **scaffold** — entry points, architecture pattern,
known anchors. Each system needs a follow-up deep-dive doc.

## Engine origin

Strings like `BonelessKFM` (Gamebryo "Key Frame Motion"), `DB_BONEINFO`,
`DB_WORLD_ANIM_DRIVERS`, `DB_WORLD_ANIM_INSTANCES`, `DB_ANIM_CURVE` confirm
this is a **Gamebryo-derived runtime** (or heavy fork). Public Gamebryo SDK
documentation, when applicable, can shortcut RE on animation/scene-graph.

## Per-frame simulation tick (already partially decoded)

`sub_67F430` is the **simulation-tick aggregator** called from the engine
pump (`sub_572040`) before `render_frame_top_level`. It calls the
sub-system ticks in order:

| Order | Function | Role | Time mode |
|---|---|---|---|
| 1 | `frame_dt_ring_update` (0x584780) | 15-slot rolling-window dt history (smoothed) | dt-based |
| 2 | `entity_transform_tick` (0x4B9F60) | Walks linked list of entities; updates orient/look-at | mixed |
| 3 | `physics_state_machine_tick` (0x4DC150) | Per-entity physics state machine (8 states); Euler-integrates pos/vel | **HARDCODED 0.003 sec** — fixed 333Hz step, NOT dt-scaled |
| 4 | `trail_subsystem_tick` (0x4D1D60) | Particle/trail lists at unk_725410 / dword_7253F4 | hardcoded 0.003 |
| 5 | `sub_4BAA40(arg)` | 4-bucket entity-update sweep (linked lists) | both 0.003 + dt |
| 6 | `sub_4D3E50(arg)` | 4-bucket entity-update sweep (different list) | both 0.003 + dt |
| 7 | `anim_update_all_tracks` (0x4E4B70) | Animation tick — walks all anim instances | dt-based |
| 8 | `sub_60EED0(dt)` | Final sub-system tick, takes seconds-dt | dt-based |

### CRITICAL caveat for FPS cap

`flt_6FFCBC = 0.003` is referenced as the fixed-step everywhere physics
integrates. Example: in `physics_state_machine_tick` (sub_4DC150),
`pos.x += vel.x * 0.003` — pure Euler with hardcoded step.

**Implication:** physics is **frame-rate dependent**. At higher FPS the
simulation runs faster (each frame = one 333Hz tick of physics). Animation
and AI use dt-based time (game_get_time_ms), so those stay correct, but
physics doesn't. The FPS limiter `Test 8` (cap 30 vs 144) is therefore
**expected to show jump-height / walk-speed differences** for any
physics-driven motion.

For the physics specifically, the engine appears to assume a *target*
~333Hz and is OK at "close enough" rates. At 60-144 FPS the deviation is
small (5-10 ticks/frame off-target) and the game is playable. At 30 FPS
the deviation is larger.

This is not a "crutch we introduced" — it's how the original engine works.
The FPS limiter is still a clean mechanism (no new hooks, no scheduler
abuse); it just exposes a pre-existing engine limitation.

For the path forward to actual 240 Hz gameplay (sim/render decoupling +
view interpolation, the Bloodborne 60 fps patch architecture), see
[`high-fps-decoupling.md`](high-fps-decoupling.md).

## 1. Level loading

### Entry chain
```
LoadLevel (screen system trigger -- TODO: locate exact entry)
  -> level_manager_state_machine
       -> level_manager_tick (0x415600, no static xrefs - vtable-dispatched per-frame)
           -> level_manager_spawn_world_grid (0x412720, called when this+3286 flag set)
               -> world_grid_spawn_scenes (0x4168F0)  [enables scenes per cell]
```

### What `world_grid_spawn_scenes` does (0x4168F0)
- Walks a `grid_w * grid_h` matrix of **48-byte cells** at `*(this+2)`.
- Each cell has a type byte (`*v6`) — values seen: 1, 2, 3, 0xF, 0x1F, 0x28, 0x29, 0x65, 0x6F, 0x79, 0x7A.
- Cell positions: `1.5` world-unit grid, origin offset `0.75`. Cell `(j, i)` is at world `(j * 1.5 + v44, i * 1.5 + v45, v46)` where `v44/v45` are computed from the player position passed in `a2`.
- Per-type dispatch:
  - **Type 2**: `scene_set_visible(scene, 1)` — main scene visibility
  - **Type 3**: `*(scene + 104) &= ~1` — clear bit 0 (the same flag we previously RE'd)
  - **Type 1**: `vtable[18](scene, 0)` — class method for visibility
  - **Type 0x28 / 0x29**: place at world coords with rotation, also calls visibility
  - **Inner sub-list per cell** (16-byte stride starting at `cell+16`): same dispatch on each entry — so each cell can have multiple child scenes.

### Open questions
- Where is `LevelPreload.txt` actually parsed? No direct xref — likely runtime-built path.
- Where do levels come from on disk? File extensions / DBL (digital-binary-large?) format.
- What's the `LevelManager` class layout? We see fields at +3284..+3380 (state, flags, world data). Need to map.

## 2. Animation

### DB tables (Gamebryo-style runtime database)
- `DB_BONEINFO` — bone metadata
- `DB_BONE_GROUP_MIRRORS` — symmetric bone pairs (for biped mirroring)
- `DB_WORLD_ANIM_DRIVERS` — drivers (input → curve selection)
- `DB_WORLD_ANIM_CURVES` — curve data (keyframes)
- `DB_WORLD_ANIM_INSTANCES` — runtime playback instances
- `DB_WORLD_ANIM_EVENTS` — discrete events (sounds, FX) on animations
- `DB_WORLD_ANIM_EVENT_STRING_TABLE` — event name → ID
- `DB_ANIM_CURVE` — single-curve animations (object-level, not skeletal)

### Per-frame tick
- `anim_update_all_tracks` (0x4E4B70) — walks all instances
- `anim_evaluate_track` (0x4E4370) — recursive (track tree); already-RE'd writer of `(scene+104) bit 0` via channel-0 ≤ 0.5

### Format
**`BonelessKFM`** = Gamebryo's KFM (Key Frame Motion). The file format is documented for vanilla Gamebryo — assume same here unless we hit something unusual.

### Open questions
- Per-instance struct layout (bone matrix array, time, blend weights).
- Driver-curve dispatch table (how a "hand wave" name resolves to bone curves).
- Track-tree structure (parent/child evaluation; recursion via `anim_evaluate_track`).
- KFM loader (find the binary parser).

## 3. Entity / NPC system

### Pattern: string-based factory
Spawn templates are **inline strings** like:
```
class=compactor;name=pickup;ai=pickup.sx;sound=pick_up;emitter=Pickup;
collectEffect=Pickup;ai/blinkOut=False;rotateDegrees=77;model_name=%s;
inventoryPickup=%s;magnetize=False;isCollectible=True;amount=1;respawn=False;
```

Other class names found: `digDugMoveable`, `digDugSwitch`, `digDugScrewball`,
`digDugAnt`, `digDugDoor`, `wilburDigDug`, `miniHamsterPlayer`, `actor`,
`compActor`. Mini-game entities (DigDug, ChargeBall) ship as separate classes.

### Hierarchy
- `entity_set_child_visible` (0x4FC540) reads a hash table (entity → children).
- Suggests an entity-with-children system; visibility cascades through hash lookup.

### KV serializer
String `|%s=%s;` at 0x6BC0BF — the engine's serializer (writes property bag back out).

### Open questions
- The factory function: takes `class=X;...` string, parses, looks up `X` in a registry, calls a class-specific ctor with the remaining KV pairs.
- Entity base class layout: vtable size, common fields (transform, visibility, parent, children list).
- Spawner architecture: `SpawnerOwnerManager` — manages NPC lifetime / respawn (`SpawnerOwnerManager_Spawn`, `SpawnerOwnerManager_KillOwners`, etc.).

## 4. AI / Scripts

### Architecture: AI = `.sx` scripts
Each entity carries an AI script via `ai=X.sx` in its spawn template
(e.g. `ai=pickup.sx`, `ai=bug.sx`, `ai=BugEnergy.sx`). The script is
loaded and run by the engine's script VM.

### Script VM
- `console_run_script` (0x589350) — runs a parsed script context
- `console_run_text_script` (0x588FB0) — parses + runs raw text
- `console_dispatch_line` — single-line dispatch
- `console_resolve_in_script_text` (0x587DE0) — variable substitution

The "console_" prefix is **misleading** — these are the script engine,
not the engine console. The console is built on top of the same VM.

### NavMesh
Functions named `AiNavMesh`, `NavMeshGetClosestPoint`, `NavMeshTestPoint`,
`NavMeshFollower`, `AiCbTargets`. Pathfinding subsystem present;
not yet RE'd.

### AI cvars
13 cvars under `ai/` group, e.g. `ai/sticky`, `ai/yaw_adjust_speed_mult`,
`ai/sidle_distance`, `ai/force_yaw`, `ai/pitch`, `ai/distance`,
`ai/missionName`, `ai/npcSubType`, `ai/npcEncounterTalkDist`. These are
runtime tuning knobs on per-entity AI behavior.

### Open questions
- `.sx` file format (probably a simple bytecode or text DSL — need to inspect a sample on disk).
- Per-entity AI tick path (when does each entity's script get its update? Likely via the entity-update sweeps in `sub_4BAA40` / `sub_4D3E50`).
- Script opcode set (assuming bytecode).
- NavMesh integration (when does the AI ask the NavMesh for a path?).

## Recommended deep-dive order

1. **Level loading** (smallest, mostly mechanical: trace file IO, identify the DBL/LSX format, write a parser if needed). Highest payoff for modding/debugging.
2. **Entity/NPC system** (unlocks everything: factory + base class layout enables targeted hooks for any subsystem).
3. **Animation** (well-understood Gamebryo lineage; mostly verify deviations from stock).
4. **AI/scripts** (largest scope; build on entity layout to find per-entity AI tick).

## Anchors created this session

| VA | Symbol |
|---|---|
| 0x412720 | `level_manager_spawn_world_grid` |
| 0x415600 | `level_manager_tick` |
| 0x4168F0 | `world_grid_spawn_scenes` (already named) |
| 0x4DC150 | `physics_state_machine_tick` |
| 0x4B9F60 | `entity_transform_tick` |
| 0x4D1D60 | `trail_subsystem_tick` |
| 0x584780 | `frame_dt_ring_update` |
| 0x67F430 | `sub_67F430` (= simulation-tick aggregator — TODO rename) |

# Level loading

How a level transitions from "user picked level X" to "world is spawned and
playable". This doc covers the LevelManager state machine, the world-grid
data structure, the cell layout, and the streaming/resource layer.

## TL;DR

1. **`LevelManager`** is a class around offset 3000+ on its instance, with state
   bytes at `this+3285`/`this+3286` and a per-frame tick at
   [`level_manager_tick`](#per-frame-tick) (`0x415600`).
2. When `this+3285` (= "load requested") is set, **`level_manager_init_load`**
   (`0x412400`) runs once: allocates the cell grid, counts entity-type pools,
   triggers the binary parser (SecuROM-protected), then transitions to state 1.
3. World data is a **2D grid of bytes** (cell-type codes); each cell becomes a
   **48-byte runtime entry** that owns a scene pointer + sub-list of children.
4. **`world_grid_spawn_scenes`** (`0x4168F0`) is the actual spawner: walks the
   cell array and dispatches per type-byte to `scene_set_visible`,
   `(scene+104) bit-clear`, or vtable methods.
5. The actual `.dbl` binary parser is **SecuROM-thunked** — we can see the
   call site (`g_securom_thunk_table_base + 180018`) but not the
   implementation. The `rr01` segment IS decrypted post-unpack, so dynamic
   instrumentation can dump whatever the thunk lands at.

## Asset format zoo

| Extension | Purpose | Examples |
|---|---|---|
| `.dbl` | Catch-all asset format ("DataBaseLite"-style) | `objects/pickup_health.dbl`, `Frontend/ro/%s.dbl`, `loadscreen/lsreveal%d.dbl`, weapons, particles, characters |
| `.mdb` | Skinned-character model database | `avatars/wilbur.mdb`, `characters/DigDugAnt.mdb` |
| `.kfm` | Gamebryo Key Frame Motion (animation) | `BonelessKFM` (referenced by name) |
| `.sx` | AI / behavior scripts | `pickup.sx`, `bug.sx`, `BugEnergy.sx` |

`.dbl` is the dominant container. Frontend UI, world geometry, characters,
particles, weapons — all `.dbl`. `.mdb` is specifically for animated/skinned
characters; everything else is `.dbl`.

## Per-frame tick

`level_manager_tick` (`0x415600`, **no static xrefs** — vtable-dispatched per
frame from the engine's screen / scene controller):

```c
void level_manager_tick(int this) {
    sub_5B39C0();                                // ?? engine pump tick
    if (*(this+3288)) {                          // world data exists
        if (*(this+3348)) goto LABEL_30;         // late state: do post-load shaders
        if (*(this+3286)) {                      // "needs to spawn world" flag
            level_manager_spawn_world_grid(this); // 0x412720
            *(this+3286) = 0;
        }
    } else {
        sub_40EAD0(this);                        // world not yet allocated
        if (!*(this+3285) || unk_7192F4) return; // load not requested or busy
        *(this+3285) = 0;
        level_manager_init_load(this);           // 0x412400
    }
    // ... post-load camera/entity bookkeeping ...
}
```

So the state machine drives:
- `*(this+3285) = 1` → trigger `level_manager_init_load` once → world data
  ready
- `*(this+3286) = 1` → trigger `level_manager_spawn_world_grid` once → cells
  visible
- `*(this+3300)` → numeric state (0/1/2/3/8 seen — LOADING / LOADED / SPAWNED
  / SUSPENDED / TEARDOWN, exact mapping TBD)
- `*(this+3348)` → late post-load flag (post-shaders / post-fade)

## Load entry: `level_manager_init_load` (0x412400)

Called once when `*(this+3285)` is set. Sequence:

```c
void level_manager_init_load(int this) {
    // 1. Reset state
    *(this+3296) = 0;  *(this+3304..3324) = 0;  *(this+3348) = 0;

    // 2. Get the camera / render-state container at this+3340
    sub_58CB50(...);
    *(this+3340) = sub_58CAD0(...);
    *(float*)(this+3332) = vtable_call(this+3340);

    // 3. Allocate 76-byte init scratch
    init_scratch = sub_5832C0(76);

    // 4. THE WORLD LOADER
    *(this+3288) = world_grid_alloc_cells(*(this+3292));   // 0x4172D0
    //              ^                      ^
    //              cell-grid pointer      world descriptor pointer
    //              (stored on LevelManager)  (set by earlier load step)

    unk_7192F4 = 1;                          // global "level loading" flag

    // 5. Set up render bounds (camera AABB based on grid + player pos)
    *(this+3340)+624 = *(this+92);           // camera-X
    *(this+3340)+628 = grid_h * 0.75;
    // ... (writes 5 floats describing the level bounds at this+3340+564)

    // 6. Bind input mapper
    int input = sub_5CB310("ControlMapper", 0);

    // 7. Mark state -> 1, fade-in screen -> 5
    *(this+3300) = 1;
    *(this+3316) = 5;

    // 8. Push the loading screen
    sub_5B03F0(0x6A7C00, 0, 0, 0);
}
```

Key insight: `*(this+3292)` is the **world descriptor pointer**, set
elsewhere (probably by the screen that picked the level). It's a 3-DWORD
struct: `[grid_w, grid_h, byte_grid_data]`.

## World grid allocation: `world_grid_alloc_cells` (0x4172D0)

```c
int world_grid_alloc_cells(int *this, int *world_desc, int a3) {
    *this           = world_desc[0];          // grid_w
    *(this+1)       = world_desc[1];          // grid_h
    int N           = world_desc[0] * world_desc[1];

    // Allocate 48*N + 4 bytes; first DWORD = N, then N x 48-byte cells.
    int *block      = sub_5832C0(48 * N + 4);
    *block          = N;

    // Construct each cell with world_cell_ctor; destructor world_cell_dtor.
    sub_62AC83(block + 1, 48, N, world_cell_ctor, world_cell_dtor);
    *(this+2)       = block + 1;              // cell array
    *(this+3)       = 0;

    // Pre-count entity-type pools from the byte grid.
    world_grid_count_pools(world_desc);       // 0x416E70

    // SecuROM-thunked binary parser does the actual cell population.
    return SECUROM_THUNK(g_securom_thunk_table_base + 180018, this, world_desc, a3);
}
```

The `48*N + 4` layout mirrors STL-vector-style: a count prefix followed by
contiguous T storage. Each cell is 48 bytes (`0x30`).

## Cell layout (48 bytes)

`world_cell_ctor` (`0x4159E0`) zeroes only the first 32 bytes — the trailing
16 are filled by the SecuROM-protected loader. Best-fit field map (from
ctor + dtor + spawn-loop usage):

| Offset | Type | Source | Purpose |
|---|---|---|---|
| `+0x00` | ptr | dtor frees | Owned allocation 1 (sub-scene list head?) |
| `+0x04` | byte+flags | spawn loop reads `*v6` | **Type code** (1/3/5-13/15/17/31/100/110/120/...) |
| `+0x08` | ptr | dtor frees, spawn reads | **Scene pointer** (passed to `scene_set_visible`) |
| `+0x0C` | ptr | dtor frees | Owned allocation 3 (decoration / FX list?) |
| `+0x10..+0x1F` | child-list | spawn iterates 4-byte stride | **Child sub-scenes** (each dispatched via same type switch) |
| `+0x18` | int=-1 | ctor | Slot ID? |
| `+0x1C` | int=-1 | ctor | Pool slot? |
| `+0x20..+0x2F` | filled by loader | not zeroed | Position / extra-properties |

The spawn loop (`world_grid_spawn_scenes`) reads cell+0x04 (type), cell+0x08
(primary scene), and walks cell+0x10..+0x1F as a list of child sub-scenes.

## Cell type codes (from `world_grid_count_pools`)

| Code | Pool-counter var | Spawn behaviour | Pool name (best guess) |
|---|---|---|---|
| `0x01` | `v6` (generic) | `scene_set_visible` or vtable[18] | Generic actor |
| `0x03` | `v14` | scene visibility, then maybe `sub_415AE0` | Door / Pickup |
| `0x05` | `v19` (++both) | -- | LOD spawn? |
| `0x06` | `v20` | NOT in generic count | Path / connector? |
| `0x07-0x0A` | bridge cells | adjacency rules (mod-4 col, etc.) | Connector tiles |
| `0x0B-0x0D` | `v21/v22/v23` | -- | ArmyAnt parts (Shard, Foot, Steam — pool names visible at 0x6A7D50/D68/CB8) |
| `0x0F` | `v17` | type-1 path | Camera / light volume? |
| `0x11` | `v6` | generic | Standard object |
| `0x1F` | `v6` + `v18` | scene visibility, + position write | Standard + extra |
| `0x28` (40) | -- (post-count) | Place at world coords + rotation | Spawn marker |
| `0x29` (41) | -- | Same with different rotation | Spawn marker variant |
| `0x64` (100), `0x6E` (110), `0x78` (120) | `v6` | generic | Standard variants |
| `0x65` (101), `0x6F` (111), `0x79` (121), `0x7A` (122) | -- | scene visibility, position write | Standard + extra position |

The naming scheme (0x64/0x6E/0x78 = 100/110/120, 0x65/0x6F/0x79/0x7A = 101/111/121/122) suggests **decimal** category numbers in the original world-editor tool (e.g., `100` = standard prop, `101` = standard prop with origin, etc.).

## Pool registration (SecuROM thunk)

`world_grid_count_pools` calls `sub_416620` (= `world_pool_register_thunk`,
SecuROM) 14 times after counting. Each call passes `(this, count, name_str_a, name_str_b, type, ...)`:

```c
*(this + 4) = world_pool_register_thunk(this, v6,  POOL_NAME_GENERIC, ...);
*(this + 5) = world_pool_register_thunk(this, v14, POOL_NAME_TYPE_3,  ...);
*(this + 6) = world_pool_register_thunk(this, v17, POOL_NAME_TYPE_F,  ...);
... // 14 total registrations
*(this + 7) = world_pool_register_thunk(this, (w-2)*(h-2), POOL_NAME_BORDER, ...);  // border cells
*(this + 15..18) = pool_register(this, 24, ..., 0x3F800000); // size 24, capacity 1.0
```

Pool names are at strings `0x6A7BE8..0x6A7DA0` — fragmentary reads include
`ArmyAntFoot`, `ArmyAntShard`, `Bug`, `BulbSmoke`, `Steam`, `Rock`, `Generic`, `Shard`. So pools are per-NPC-type (army ants drop limbs, bugs drop steam, etc.)
plus generic categories.

## World grid spawn: `world_grid_spawn_scenes` (0x4168F0)

Already RE'd in the earlier scene-visibility investigation.
Recapping the per-cell dispatch for completeness:

```c
for (each cell at (j, i) in grid_w x grid_h) {
    pos_x = j * 1.5 + (player_x - grid_w * 0.75 + 0.75);
    pos_y = i * 1.5 + (grid_h * 0.75 + player_y - 0.75);
    pos_z = player_z + 0.75;

    cell = grid[i*w + j];
    sub_415940(cell);                      // populate cell (SecuROM thunk)

    switch (cell->type) {
    case 0x02: scene_set_visible(cell->scene, 1); break;
    case 0x03: cell->scene->flags &= ~1; break;       // (scene+104) bit 0 clear
    case 0x01: cell->scene->vtable[18](cell->scene, 0); break;
    case 0x28/0x29: place_at_world_coords + scene_visibility; break;
    case 0x1F/101/111/121/122: position write + visibility; break;
    }

    // Walk inner sub-list at cell+0x10 (entries every 4 bytes).
    for (sub = cell+0x10; valid; sub += 4) {
        // Same dispatch on sub->type (case 1/2/3).
    }
}
```

## Resource manager

Script-bound functions (callable from `.sx`):
- `ResourceManager_LoadFile(path)`
- `ResourceManager_UnloadFile(path)`
- `ResourceManager_IsLoaded(path)`
- `ResourceManager_IsLoadPending(path)`

Plus streaming controls:
- `ForceStreamingOn`, `ForceStreamingOff`, `LockoutStreaming`
- `IsActorEligibleForStreaming`

The string-table entries for these have **zero IDA xrefs** — they're
SecuROM-protected (registered via the thunked script-binding registrar).
The implementation lives in encrypted code. Runtime instrumentation
(IAT-style hook on the script-binding registration call site) can pin
their addresses.

## What's blocked by SecuROM

Three layers we can't read statically:

1. **`world_cell_populate_thunk`** (`0x415940` → SecuROM): per-cell binary
   data parser. Reads from a `.dbl`-derived buffer, fills cell+0x20..+0x2F
   plus cell-owned allocations.
2. **`world_pool_register_thunk`** (`0x416620` → SecuROM): pool allocator
   internals.
3. **The SecuROM thunk in `world_grid_alloc_cells`** (`g_securom_thunk_table_base + 180018`): top-level binary parser that walks the byte-grid + populates each cell. This is THE level loader's hot core.

For runtime RE: the rr01 segment IS decrypted in memory (per memory). A hook
at any of these thunks can dump caller-saved args and the resolved return
addresses — that gives the actual SecuROM-protected function VAs in the
process image.

## What's left to investigate

1. **Trace the load trigger**: where does `*(LevelManager+3285)` get set?
   Likely from a screen factory ("Loading" screen or "PlayLevel" screen
   button). The screen system is already RE'd; the bridge is finding the
   per-screen code that calls `LevelManager.beginLoad()` (vtable-style).
2. **Identify the world descriptor source**: `*(LevelManager+3292)` is the
   world descriptor (grid_w/h + byte-grid pointer). Where does this pointer
   come from? Possibly from the level select screen via `LevelName_*`
   loc-key → resource lookup, or from `LevelPreload.txt`.
3. **`.dbl` format**: the file is a catch-all binary container. Inspecting
   a sample on disk + writing a small dumper would unblock format-aware
   tooling. The header probably has a magic + version + table-of-contents.

## Anchors created this session

| VA | Symbol |
|---|---|
| `0x412400` | `level_manager_init_load` |
| `0x412720` | `level_manager_spawn_world_grid` |
| `0x415600` | `level_manager_tick` |
| `0x4159E0` | `world_cell_ctor` |
| `0x415A00` | `world_cell_dtor` |
| `0x415940` | `world_cell_populate_thunk` (SecuROM) |
| `0x416620` | `world_pool_register_thunk` (SecuROM) |
| `0x416E70` | `world_grid_count_pools` |
| `0x4168F0` | `world_grid_spawn_scenes` (already named) |
| `0x4172D0` | `world_grid_alloc_cells` |

## See also

- [`systems-survey-2026-05-06.md`](systems-survey-2026-05-06.md) — top-level survey
- [`render-pipeline.md`](render-pipeline.md) — how spawned scenes get rendered
- [`optimization-systems.md`](optimization-systems.md) — `(scene+104)` flag (= the type-3 bit-clear above)

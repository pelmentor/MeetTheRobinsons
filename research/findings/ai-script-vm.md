# AI / `.sx` Script VM + NavMesh

How `.sx` AI scripts are parsed, dispatched, registered, and how they
drive entity behavior. Plus the NavMesh API surface.

## TL;DR

1. **Architecture is event-driven, not tick-driven.** Scripts register
   `OnX` callbacks; the engine fires them when X happens. There is **no
   per-frame "tick all AI scripts" pass**. C++ entity classes do the
   per-frame work and emit events to script handlers.
2. **`.sx` is a text-based command language** — line-based, `;`/`\n`
   separators, `\\` continuation, `#` comments. NOT bytecode. Each line
   is a single command parsed at runtime.
3. **Bidirectional registry**: each (name, callback) binding lives in a
   32-byte node on TWO doubly-linked lists (one by name, one by
   callback). Lookup by name OR by callback is O(chain length).
4. **Master dispatcher is SecuROM-protected**:
   `script_command_dispatch_giant` (`0x5B40C0`, 11KB) handles both
   entity-property application and script command invocation. Most of
   the actual command handlers (Spawn, NavMesh, ResourceManager, etc.)
   are bound to it via SecuROM-thunked registrations.
5. **The two "entity sweeps" in the simulation tick are PARTICLES, not
   AI** — `particle_buckets_sweep_a/b` walk 10 buckets of FX particles,
   each calling `particle_update_tick` (also hardcoded 0.003 step).
   Earlier docs called these "entity update sweeps"; that was wrong.

## Script language

### Line format

```
# this is a comment
command arg1 arg2 arg3
set foo bar
say "hello world"
\
  continued from previous line via backslash; whitespace skipped after \
context view
```

Lines separated by `;` or `\n`. Inside a command, `/` is rewritten to
`\\` (backslash) — likely so paths don't interfere with continuation.
Tabs become spaces. Starting `#` is a comment (skipped).

### Tokenizer — `console_tokenize_token` (0x587740)

Splits `a1` into `a3` (output buffer, capped at `a4` chars) using
`a2` as the delimiter set (multi-character; any char in `a2` is a
delimiter). Optionally writes a "delimiter found" boolean to `a5`.
Used everywhere — for splitting whitespace-delimited words AND for
splitting by specific chars (e.g. `:` for context-name-vs-cvar).

### Multi-line runner — `console_run_text_script` (0x588FB0)

Walks raw text:
- Lazily allocate work buffer in `state[6/7]` (size + ptr) sized to
  the longest script ever run
- For each line:
  - Skip leading whitespace
  - Translate per-char (`\t` → space, `/` → `\\`, `\\` → continuation)
  - Trim + normalize via `sub_5871F0`
  - If non-empty: `console_dispatch_line(state, line)`

### Dispatcher — `console_dispatch_line` (0x588DB0)

```c
char console_dispatch_line(state, line) {
    if (line[0] == '#') return 1;                  // comment
    first_token = console_tokenize_token(line, " \t", buf, 255);
    // command-table at state[10..], state[11] entries, 12-byte each:
    //   { handler_fn (4B), name_ptr (4B), reserved (4B) }
    for (i = 0; i < state[11]; ++i) {
        if (strcasecmp(buf, table[i].name) == 0)
            return table[i].handler_fn(state, buf, rest);
    }
    // Fallback: variable get/set or context-bound command
    rest = console_tokenize_token(buf, ":", varbuf, 255, &has_colon);
    if (has_colon)
        return console_set_or_invoke_var_cmd(varbuf, rest, line_remainder);
    else
        return console_set_or_invoke_var_cmd(NULL, buf, line_remainder);
}
```

So **the dispatcher resolves a token in three layers**:
1. **Built-in command** in `state[10]` table (linear scan)
2. **`name:cmd` notation** — the `:` means "invoke `cmd` on context `name`"
3. **Variable get/set** — `set foo` / `foo` returns the value

The state's command table at `state[10]` is sized at `state[11]` —
populated via the registry path below.

### Coroutine runner — `sub_588AC0`

Used when a script is in "compiled / bound" mode (`state[8].count > 0`):
```c
while (!sub_58A590(parsed_script))
    if (++attempts >= state[8].count) return 0;
return 1;
```
Walks the bound handler chain until one returns success. So scripts can
have multiple alternative handlers and the engine picks the first
matching one — like a behavior tree's selector node.

### Variable resolver — `console_resolve_in_script_text` (0x587DE0)

Resolves `$name` references inside a script. Two modes:
- **Scoped lookup**: walk the scope chain via `sub_58A7F0` (encrypted)
- **Inline scan**: re-tokenize the entire script text, find a line
  whose first token matches the requested var name, return the rest
  of that line as the value

So scripts can have **inline variable definitions** at the top:
```
target_distance 30.5
target_pitch 0.0
```
And later code can read `$target_distance` to get `30.5`.

## Registry

### Name interning — `script_name_intern` (0x59E080)

```c
int script_name_intern(const char *name) {
    handle = sub_5D3730(name, len);          // hash table lookup
    if (!handle || !*handle) {
        // Allocate (24 + len) bytes (16-byte aligned), put name string at +20
        node = sub_582DF0(len + 24, 0x10, 1);
        sub_5A60F0(); sub_5A5A60(...);       // lock
        sub_59D2D0(name);                     // (init?)
        if (existed)
            sub_59D930(name, len, node, node + 20);
        else
            handle = sub_5EDF50(name, len, node, node + 20);
        sub_5A5A80(...);                      // unlock
    }
    return *handle;
}
```

Names are de-duped via a global hash table (lookup at `sub_5D3730`).
Returned handle is a 24-byte header followed by the name string. All
script bindings reference names by interned handle, not by string —
so equality is a pointer compare.

### Node insertion — `script_register_node` (0x59D380)

Allocates a **32-byte node** and chains it into TWO doubly-linked lists:

| Offset | Field |
|---|---|
| `+0x00` | name handle (interned) |
| `+0x04` | callback arg (or "command target") |
| `+0x08` | scope (the `this` of the registering script state) |
| `+0x0C` | prev-by-callback |
| `+0x10` | next-by-callback |
| `+0x14` | prev-by-name |
| `+0x18` | next-by-name |
| `+0x1C` | padding |

Both lists are visited from a head argument (`a2` = name list head,
`a3` = callback list head). When the node is created, its `*a2` becomes
its prev, the head pointer is updated to point at the node, etc. —
classic intrusive doubly-linked list.

Why bidirectional?
- **Lookup by name**: "what callbacks are registered for `OnPickup`?"
- **Lookup by callback**: "given this entity, what events does it
  listen to?" (used for cleanup on entity destruction).

### Public registration — `script_register_command` (0x59E180)

```c
int script_register_command(scope, name, callback_arg) {
    handle = script_name_intern(name);
    return script_register_node(scope, handle, callback_arg);
}
```

Used everywhere entities expose script-callable commands. Each entity
class registers its commands at construction (e.g.,
`entity_init_with_model` registers `"ToggleRagDollPhysics"`).

## Master dispatcher — `script_command_dispatch_giant` (0x5B40C0)

11KB string-keyed function. Acts as **both**:
- **Entity property applier**: when applying a property bag like
  `magnetize=False; rotateDegrees=77; isCollectible=True`, each KV is
  dispatched here. The `key` string finds the right C++ field setter.
- **Script command handler**: when a `.sx` script line invokes a
  command (e.g. `set rotateDegrees 90`), the dispatcher routes it.

The size + thunked internals (calls to `entity_property_get_thunk`,
class-registry SecuROM thunks) make full static RE impractical. Runtime
hooking is the right answer: log every (key, value) pair seen for ~5
minutes of gameplay → empirical schema of the entity DSL.

## Bound script commands seen in code

These names appear as inline strings — bindings registered through
`script_register_command` (or its SecuROM-thunked variants).

### Spawning / NPC lifecycle
- `SpawnerOwnerManager`
- `SpawnerOwnerManager_Spawn`
- `SpawnerOwnerManager_SpawnDelay`
- `SpawnerOwnerManager_KillOwnersAndPendingSpawns`
- `SpawnerOwnerManager_HasOwnersOrPendingSpawns`
- `SpawnerOwnerManager_GetAliveRevivable`
- `SpawnerOwnerManager_GetNumAliveRevivables`
- `Hud_SetRespawnTimer`
- `HUDSetRespawnTimer`

### Resource management
- `ResourceManager_LoadFile`
- `ResourceManager_UnloadFile`
- `ResourceManager_IsLoaded`
- `ResourceManager_IsLoadPending`
- `ForceStreamingOn`, `ForceStreamingOff`, `LockoutStreaming`
- `IsActorEligibleForStreaming`

### Animation / model
- `ToggleRagDollPhysics`
- `UpdateBonelessKFMInstances`
- `PlayWilburFaceAnim`, `StopWilburSecondaryAnims`
- `ResetActionIFAnim1AndHandlers`

### NavMesh / AI
- `AiNavMesh`
- `AiCbTargets`
- `NavMeshGetClosestPoint`
- `NavMeshTestPoint`
- `NavMeshFollower`

### Sound / cutscene
- `PlayAttachedSound`, `PlayStationarySound`, `playSound`
- `PlayCutScene`, `PlayCutSceneNoAbort`

### Halo / FX
- `HaloSetFadeOutTime`, `HaloSetFadeInTime`

All of these have **zero IDA xrefs** — registered via SecuROM-thunked
calls. Runtime hooks on `script_register_command` would dump every
(name, scope, callback) tuple in ~5 minutes.

## NavMesh

Five script-callable functions form the NavMesh API:

| Command | Purpose |
|---|---|
| `NavMeshGetClosestPoint(x, y, z)` | Snap an arbitrary point to the closest walkable spot |
| `NavMeshTestPoint(x, y, z)` | Returns whether the point is on a walkable area |
| `NavMeshFollower` | (registered AI behavior class) |
| `AiNavMesh` | (script handle for the global navmesh) |
| `AiCbTargets` | (script handle for AI callback targets) |

The actual navmesh data (mesh polys, adjacency, search) is encrypted —
the implementations live behind SecuROM thunks. Runtime hook on the
registration site of `NavMeshGetClosestPoint` would pin its
implementation address in the decrypted `rr01` segment.

## AI cvars (per-entity tuning)

13 cvars under `ai/`:
- `ai/sticky`, `ai/sticky_enable`
- `ai/yaw_adjust_speed_mult`, `ai/yaw_adjust_speed_enable`
- `ai/speed_threshhold`, `ai/speed_threshhold_enable`
- `ai/sidle_distance`, `ai/sidle_distance_enable`
- `ai/force_yaw`, `ai/force_yaw_enable`
- `ai/pitch`, `ai/pitch_enable`
- `ai/distance`, `ai/distance_enable`
- `ai/missionName`, `ai/npcSubType`, `ai/npcEncounterTalkDist`

These are read at runtime via `entity_property_get_thunk("ai/...")`
when a script wants to know an entity's AI state. Each follows the
`ai/X` + `ai/X_enable` (boolean gate) pattern — common in data-driven
AI to enable/disable individual influences.

## Per-frame integration

**There is NO per-frame "tick all AI scripts" pass.**

The simulation tick aggregator (`simulation_tick_aggregator @ 0x67F430`)
calls 8 sub-system ticks; **none is an AI script tick**. The two
"entity sweep" functions previously suspected were:
- `particle_buckets_sweep_a` (`0x4BAA40`) — 10 buckets of particle FX,
  walks each list calling `particle_update_tick`
- `particle_buckets_sweep_b` (`0x4D3E50`) — sister sweep, slightly
  different bucket offsets, walks list via `+200` offset

Both confirmed by decompiling the per-list callback (`particle_update_tick`
@ `0x4D9230`): it's a particle physics tick — lifetime, position,
rotation, color/alpha — NOT a script tick.

So AI runs purely **event-driven**:
1. C++ entity classes do per-frame work (move, animate, collide).
2. When something happens (collision, timer fires, target enters
   range, animation event hits), the C++ code calls
   `script_command_dispatch_giant` with the event name + args.
3. The dispatcher walks the bound-name list in the registry.
4. Matching script handlers run via `console_run_script` /
   `console_dispatch_line`.
5. Script handlers can call back into C++ via
   `script_register_command`-bound callbacks.

That's why `.sx` files contain things like
```
event OnPickup
    set isCollectible False
    PlayAttachedSound pick_up
    SpawnerOwnerManager_Spawn FXPuff
```
— event handlers, not tick logic.

## What's blocked by SecuROM

- `script_command_dispatch_giant` internals (class registry, handler
  table, full command list)
- Most named handler bodies (Spawn, NavMesh*, ResourceManager_*, etc.)
- `script_name_intern`'s underlying hash store (`sub_5D3730`,
  `sub_5EDF50`)
- The script-binding registration call sites (so the *registration*
  itself is hidden, not just the handler bodies)

For a complete AI dump: hook `script_register_command` (and trace its
caller's return into the registration thunks). After 5 minutes of
gameplay across all level types, you have:
- The full set of script command names
- Each name's scope (entity class)
- Each name's callback address (in decrypted code)

## Anchors created this session

| VA | Symbol |
|---|---|
| `0x4D9230` | `particle_update_tick` (formerly thought to be AI-related) |
| `0x4BAA40` | `particle_buckets_sweep_a` |
| `0x4D3E50` | `particle_buckets_sweep_b` |
| `0x59D380` | `script_register_node` (32B node insert into bidirectional list) |
| `0x59E080` | `script_name_intern` (de-dup name → 24B handle) |
| `0x59E180` | `script_register_command` (already named) |
| `0x587740` | `console_tokenize_token` (already named) |
| `0x587DE0` | `console_resolve_in_script_text` (already named) |
| `0x588DB0` | `console_dispatch_line` (already named) |
| `0x588FB0` | `console_run_text_script` (already named) |
| `0x589350` | `console_run_script` (already named) |
| `0x5B40C0` | `script_command_dispatch_giant` (already named, 11KB) |

## See also

- [`systems-survey-2026-05-06.md`](systems-survey-2026-05-06.md) — top-level overview
- [`entity-system.md`](entity-system.md) — entity property bag (the script's primary state surface)
- [`animation-system.md`](animation-system.md) — channel-0 visibility cascade and `UpdateBonelessKFMInstances`
- [`level-loading.md`](level-loading.md) — `.sx` files in the asset zoo

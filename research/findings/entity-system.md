# Entity / NPC system

How entities (objects, characters, NPCs, pickups, projectiles) are
defined, instantiated, ticked, and connected to AI scripts and animations.

> **2026-05-10 CORRECTION (Phase 0A RE):** The factory claim in this doc was wrong.
> `sub_5B40C0` ("script_command_dispatch_giant") is the **scripted-trigger / cutscene
> command dispatcher** (keyed on `commandType` against `actorActivate`,
> `addInventory`, `changeMusic`, `fadeIn`, `flicker`, `checkpointStart`, …) —
> **NOT** the entity factory. The IDB name was correct.
>
> The real entity factory is **`entity_factory_construct` at `0x5B96F0`**
> (~310 bytes, fully decompilable). Class registry head is **`g_class_registry_head`
> at `0x7429C8`** with 17 classes registered (`protagonist`, `actor`, `compActor`,
> `plant`, `triggerbox`, `playerActor`, `ColorGun`, `testPlayer`, `miniHamsterPlayer`,
> 6 DigDug variants, `wilbur`, `wilburDigDug`, `digdug`). The "50+ classes" claim
> below was wrong — there are 17. Full writeup:
> [coop-phase-0a-entity-factory-2026-05-10.md](coop-phase-0a-entity-factory-2026-05-10.md).

## TL;DR

1. **Pure data-driven.** Entities are described by **string KV bags** like
   `class=compactor;name=pickup;ai=pickup.sx;sound=pick_up;model_name=...`.
   The engine reads each property at runtime by string key.
2. **Property accessor is SecuROM-thunked.** `entity_property_get_thunk`
   (`0x4B8F00`, ~150 callers) is the universal "get property by name"
   function, but its body is encrypted. Static RE can map *who reads what*
   but not *how the bag is stored*.
3. **The factory dispatcher is also encrypted.** `script_command_dispatch_giant`
   (`0x5B40C0`, 11KB) is a huge string-keyed dispatcher used for both
   script commands and entity-property application. It internally calls
   SecuROM thunks for the actual class registry and instance creation.
4. **Entities are heavyweight.** Different entity classes have different
   layouts (320 B "effect" entities to 1080+ B "character" entities), all
   sharing a common set of fields (visibility byte, child list, vtable).
5. **A registry hash table** at `dword_7427C0` (mask `dword_6CBD8C`)
   resolves `entity_id → entity_ptr` in O(1). Used by visibility cascades
   and script-callable lookups.

## Spawn template format

Inline strings in the binary, e.g.:
```
class=compactor;name=pickup;ai=pickup.sx;sound=pick_up;
emitter=Pickup;collectEffect=Pickup;ai/blinkOut=False;
rotateDegrees=77;pickupSelection=Inventory Pickup;
pickupType=inventory;model_name=%s;inventoryPickup=%s;
magnetize=False;isCollectible=True;amount=1;respawn=False;
```

Tokens are `key=value;` separated by `;`. Values can be:
- Identifiers: `compactor`, `inventory`
- Strings: `Inventory Pickup`
- Booleans: `True`/`False`
- Numbers: `77`, `1`
- Format placeholders: `%s` (substituted at runtime by the caller)

The serializer is `|%s=%s;` (string at `0x6BC0BF`) — that's how property
bags are written back out (e.g. for save games).

### Entity classes seen in inline templates

`compactor`, `digDugMoveable`, `digDugSwitch`, `digDugScrewball`, `digDugAnt`,
`digDugDoor`, `wilburDigDug`, `miniHamsterPlayer`, `actor`, `compActor`.

Mini-game entities (DigDug, ChargeBall) ship as their own class names
prefixed `digDug*`. The base class is probably `actor` (used as a fallback
class for spotlights, decorations).

### Standard property keys seen across templates

| Key | Purpose |
|---|---|
| `class` | C++ class name (resolves via factory dispatch) |
| `name` | Instance label |
| `model_name` | Path to `.mdb`/`.dbl` model |
| `mdb1` | Alt model key (skinned characters) |
| `ai` | Path to `.sx` AI script |
| `sound` | Sound to play on spawn |
| `emitter` | Particle emitter on spawn |
| `collectEffect` | Particle effect on collection |
| `pickupSelection` | Inventory category |
| `pickupType` | `inventory`, `health`, `energy`, etc. |
| `inventoryPickup` | Inventory item granted |
| `rotateDegrees` | Visual rotation speed |
| `magnetize` | Auto-pickup-toward-player |
| `isCollectible` | Player can collect |
| `amount` | Quantity granted on pickup |
| `respawn` | Respawns after collect |
| `ai/blinkOut`, `ai/sticky`, `ai/pitch`, `ai/distance`, etc. | AI tuning sub-properties |

## Property accessor

**`entity_property_get_thunk` (0x4B8F00)** — `SecuROM_THUNK(thunk_table + 178618)`.

```c
int entity_property_get_thunk(this, key_or_unused, val_or_unused);
// returns: pointer to property value (string, or pointer-to-bytes)
```

Used 150+ times, e.g. in `mdb_parse_model`:
```c
v2 = entity_property_get_thunk("mdb1");      // try animated model key
if (!v2) v2 = entity_property_get_thunk("model_name");  // fallback
if (!v2 || strcmp(v2, "none") == 0) return 0;
sub_57D450(this+4, &Default, v2, "rgd");     // load resource
```

The `this` argument plus the active context provides the property bag —
each entity has its own bag, and the function is called from per-entity
contexts.

### Top callers (entity-related code regions)

| Caller VA | Size | Probable role |
|---|---|---|
| `script_command_dispatch_giant` (0x5B40C0) | 11KB, 30+ calls | Master dispatcher (entity factory + script handlers) |
| `0x434B90..0x434F7B` | 4 calls | Entity vis-cascade init |
| `0x456310..0x45648A` | 5 calls | (likely a class-specific ctor) |
| `sub_542DB0` (0x542DB0) | 9 calls | (likely a class-specific ctor) |
| `sub_5445C0` (0x5445C0) | 4 calls | (likely a class-specific ctor) |
| `0x4E3FC0..0x4E40CB` | 8 calls | Animation track lookup (anim+entity boundary) |
| `mdb_parse_model` (0x5FB110) | 2 calls | Skinned-model loader |

The set of callers is the *vocabulary of property keys* — every property
read is a call to this thunk with the key string in args.

## Entity layout (hetereogeneous)

There is no single "Entity base class" with one canonical layout.
Different entity types use overlapping field offsets but with different
semantics. Common offsets observed:

| Offset | Type | Used by | Purpose |
|---|---|---|---|
| `+0x00` | vtable* | all | C++ vtable pointer |
| `+0x04` | ptr | most | Parent / outer scene |
| `+0x14` | uint32 | (registry hash key) | **Entity ID** (used to find self in `dword_7427C0`) |
| `+0x18` | int | many | Reserved / pad |
| `+0x14` | ptr/count | "effect" entities | Child list + count at +0x18 |
| `+0x3C/+0x40` | ptr/count | "effect" entities | FX-child list + count |
| `+0x50` | byte | all | **Visibility flag** (0=hidden, 1=visible) |
| `+0x68` | dword | scenes | Scene flags (bit 0 = `(scene+104) bit 0` we know) |
| `+0x6C` | callback ptr | physics-enabled | Damage / physics callback |
| `+0x90` | ptr | physics-enabled | Allocated physics body |
| `+0x88` | byte | physics-enabled | Physics flag |
| `+0x158` | (animated entities) | bone matrices? | Animation state pointer |
| `+0x410` | ptr | "character" entities | Metadata / loadout pointer |
| `+0x430` | byte | "character" entities | Visibility-pending flag |
| `+0x438` | byte | "character" entities | Last-applied visibility |

So an entity's class determines which subset of these fields exist.
Effect entities are ~100-300 bytes; pickups ~500 bytes; characters
1080+ bytes.

## Visibility cascade — `entity_set_visibility_cascade` (0x434B90)

Concrete example showing how an entity's vis state propagates:

```c
void entity_set_visibility_cascade(int this, byte on, int unused) {
    if (on != *(this+80)) {                            // changed?
        // Cascade UP to parent
        vtable_call(this+4, on);                       // parent->setVis(on)

        // Cascade DOWN to FX children
        for (i=0; i<*(this+64); ++i) {
            vtable_call(*(this+60)[i], on);
        }

        // Toggle (scene+104) bit 0 on each direct child scene
        for (j=0; j<*(this+24); ++j) {
            scene = **(this+20)[j];
            scene->flags = on ? (flags & ~1) : (flags | 1);
        }

        *(this+80) = on;                               // commit
    }
}
```

This is the **same `(scene+104) bit 0`** we ruled out for *corner*
culling — but it IS used for entity hide/show via script. Confirms the
flag is script-driven only.

## Entity registry hash table

`dword_7427C0` is the entity registry — a hash table.
- Bucket index = `entity_id & dword_6CBD8C` (so the mask is `count - 1`,
  count is power of 2)
- Each bucket is a linked list of 4-DWORD entries:
  - `+0x00` = entity_ptr (or scene_ptr)
  - `+0x04` = entity_id
  - `+0x08` = (unused / reserved)
  - `+0x0C` = next-in-bucket

Used by `entity_set_child_visible` to look up a child entity by ID and
toggle a sub-scene's visibility bit. Probably also by script
"FindEntityByID" and similar lookups.

## Script bindings — `script_register_command` (0x59E180)

Pattern seen in `entity_init_with_model`:
```c
script_register_command(this+53, "ToggleRagDollPhysics", *(this+1)+328);
```

So `script_register_command(scope, name, callback_arg)` ties a string
command name to a callback and an arg. `sub_59E080(name)` resolves /
hashes the name; `sub_59D380(callback_arg, hash)` registers it. Each
entity class registers its supported commands at init.

This is the bridge from `.sx` AI scripts to entity-class C++ code.
`SpawnerOwnerManager_Spawn`, `ResourceManager_LoadFile`,
`ToggleRagDollPhysics`, etc. — all registered through this path.

## SpawnerOwnerManager

Script-bound functions (registered via `script_register_command`):
- `SpawnerOwnerManager_Spawn(...)` — spawn one
- `SpawnerOwnerManager_SpawnDelay(...)` — schedule delayed spawn
- `SpawnerOwnerManager_KillOwnersAndPendingSpawns()` — clear all
- `SpawnerOwnerManager_HasOwnersOrPendingSpawns()` — check
- `SpawnerOwnerManager_GetAliveRevivable()` — find downed NPC
- `SpawnerOwnerManager_GetNumAliveRevivables()` — count
- `Hud_SetRespawnTimer`, `HUDSetRespawnTimer` — UI

All bindings have **zero IDA xrefs** (the registration call site is
SecuROM-thunked). Runtime instrumentation needed to find their
implementations in the decrypted rr01 segment.

## What `script_command_dispatch_giant` (0x5B40C0) does

The 11KB function is a **string-keyed dispatcher** (signature
`(this, ?, const char* a3, ?, ?, char)`) — `a3` is the property name
or command name. It branches across hundreds of cases, each doing one
of:
- Read another property via `entity_property_get_thunk`
- Compare strings (`sub_63599D` = strcmpi-ish)
- Call a class method on `this` (vtable dispatch)
- Look up the class registry (SecuROM thunks)

This is BOTH the entity-property-application path (when spawning an
entity, it walks the KV bag and applies each value via this dispatcher)
AND the script command handler (when AI scripts call `set foo bar` or
similar, it goes here).

The function's size + opacity (11K of branched dispatch + thunked
registry calls) makes it impractical to fully RE statically. The
*right* approach is runtime instrumentation:

1. Hook `entity_property_get_thunk` and log every key string seen.
2. Hook `script_register_command` and log every binding.
3. After 5 minutes of gameplay across all level types, the union of keys
   + bindings is the complete entity DSL.

## NavMesh anchors

Script bindings exposed:
- `NavMeshGetClosestPoint(x, y, z)` — snap a point to the navmesh
- `NavMeshTestPoint(x, y, z)` — is point on a walkable area
- `NavMeshFollower` — registered AI behavior class
- `AiCbTargets`, `AiNavMesh` — script-callable AI introspection

NavMesh implementation is SecuROM-thunked. Format and runtime data
structure not yet RE'd.

## What's blocked by SecuROM

| Function | Role | Workaround |
|---|---|---|
| `entity_property_get_thunk` (0x4B8F00) | Property bag GET | Hook → log key strings, build schema empirically |
| `script_command_dispatch_giant` (0x5B40C0) — internal calls | Class registry, instance create | Hook → log class names + ctor returns |
| `script_register_command` (0x59E180) — internal `sub_59E080`, `sub_59D380` | Command name hashing + table insert | Hook → log every (name, scope, callback) tuple |
| `world_cell_populate_thunk` (0x415940) | Per-cell binary parser (from level loading doc) | Hook → log cell-byte → entity dispatches |

Once these four hooks are running, the entire entity DSL is observable
at runtime within minutes of gameplay.

## What's left to investigate

1. **Class registry**: where is the `class_name → ctor_func` map? Almost
   certainly behind the SecuROM thunks called from
   `script_command_dispatch_giant`.
2. **Per-class layouts**: the `compactor`, `actor`, `digDugAnt` etc.
   ctors are likely in the `0x456310/0x542DB0/0x5445C0` region (medium-
   sized callers of property_get).
3. **Entity tick path**: when does each entity get its per-frame update?
   The two 4-bucket entity-update sweeps in the simulation tick
   (`sub_4BAA40` @ `0x4BAA40` and `sub_4D3E50` @ `0x4D3E50`) are the
   prime suspects — they walk linked lists with vtable calls.
4. **Property bag storage**: how is the KV bag serialized in memory? A
   `name_hash → value_offset` table per entity, probably. The thunks
   own this layout.

## Anchors created this session

| VA | Symbol |
|---|---|
| `0x434B90` | `entity_set_visibility_cascade` |
| `0x4B8F00` | `entity_property_get_thunk` (SecuROM) |
| `0x59E180` | `script_register_command` |
| `0x5B40C0` | `script_command_dispatch_giant` (11KB string-keyed dispatcher) |
| `0x5F1FC0` | `entity_init_with_model` |
| `0x5FB110` | `mdb_parse_model` |
| `dword_7427C0` | `g_entity_registry_buckets` (TODO rename in IDB) |
| `dword_6CBD8C` | `g_entity_registry_mask` (TODO rename in IDB) |

## See also

- [`level-loading.md`](level-loading.md) — what spawns these entities (cell type → entity class)
- [`systems-survey-2026-05-06.md`](systems-survey-2026-05-06.md) — top-level survey
- AI subsystem doc (TODO) — how `.sx` scripts drive these entities
- Animation deep-dive (TODO) — `mdb_parse_model` plus DB_BONEINFO/DB_WORLD_ANIM_*

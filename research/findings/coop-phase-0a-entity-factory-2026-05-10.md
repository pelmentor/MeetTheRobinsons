# Coop Phase 0A — Entity factory RE (2026-05-10)

**Status:** Phase 0A complete. The 2–3 week estimate from the v2 plan was extreme overshoot — the recon doc misidentified `sub_5B40C0` as the factory; the actual factory is a small, well-formed function. Total RE time this session: ~1 hour.
**Governing rule:** [feedback_no_crutches.md (RULE №1)](../../memory/feedback_no_crutches.md).
**Plan:** [coop-multiplayer-plan-2026-05-10.md](coop-multiplayer-plan-2026-05-10.md).

---

## TL;DR for the next phase

- `sub_5B40C0` is **NOT** the entity factory. It's the cutscene/scripted-trigger command dispatcher (keyed on `commandType` against `actorActivate`, `addInventory`, `changeMusic`, `fadeIn`, `flicker`, `checkpointStart`, etc.). The IDB name `script_command_dispatch_giant` was correct; entity-system.md's "factory dispatcher" claim was wrong.
- The real entity factory is **`entity_factory_construct` at `0x5B96F0`** (~310 bytes, fully decompilable). Reads `class` from a bag, looks up the class in a singly-linked list registry, calls `registry_entry->vtable[+4](this=entry, bag)` → returns the new entity pointer. 17 callers, all wrapper-spawners.
- Class registry head: **`g_class_registry_head` at `0x7429C8`** (was `dword_7429C8`).
  - 17 classes registered at the snapshot. Full list below.
  - The PLAYER CLASS is **`protagonist`** (chain head). `"player"` is the entity-NAME, not the class.
- Bag KV-store internals: **HASHED keys**. `bag_merge_into` (`0x4B95A0`) interns key/value strings via `string_intern_hash` (`sub_5D3E30`) before insert. Lookup by string is `key_hash → value_hash` matching.
- The bag-from-template parser **`bag_init_from_template` at `0x4B9750`** is a stolen-byte thunk that resolves at runtime to address `0x1007700` (outside the static IDB image). At runtime, mod code can call it directly — same as `entity_property_get_thunk` which the existing `entity_kv` wrapper already calls successfully.

**This unblocks Phase 2 (second-player spawn).** The architecture for a "spawn another protagonist named player2" call is:

```cpp
// Pseudo-code; exact ABI for a1/a2/a4/a5 still needs runtime verification.
void* bag_handle = nullptr;
bag_init_from_template_THUNK(&bag_handle, "class=protagonist; name=player2; …");
void* entity = entity_factory_construct(/*ebp*/ default_name_ptr,
                                        /*esi*/ default_misc_ptr,
                                        /*bag*/ &bag_handle,
                                        /*int_a4*/ 0,
                                        /*float_a5*/ 0.0f);
// On success: entity points at a fully-constructed protagonist instance,
// already linked into transform list, ready for input-binding (Phase 2's other half).
```

What's still unknown for the call: the semantic content of the two register-passed args (ebp = `a1`, esi = `a2`). They flow into `entity_property_get_thunk(bag, key, default)` as the THIRD arg (the default value when the property is absent from the bag). Passing 0/0 should be safe for a fresh template — most properties are present in the template and won't fall through to the default.

---

## Reorientation: `sub_5B40C0` is a scripted-trigger dispatcher, not the factory

`sub_5B40C0` ("script_command_dispatch_giant", ~11 KB, ~74 callees, 209 string refs) **dispatches on the `commandType` property** read from a bag, against literals like:

```
None, actorActivate, actorDeactivate, addInventory, changeMusic,
checkpointStart, checkpointFinish, collisionOff, collisionOn,
constRot, constVel, fadeIn, fadeOut, flicker, …
```

These are **scripted-event commands** (the .sx-script verb VM), not entity classes. The first property read is `commandType`, then sub-properties are read inside each branch (e.g. `changeMusic` reads `strTrackName`, `nTrackLooped`, `nTrackQueued`, `fFadeOutPrev`, `fFadeInThis`).

The previous recon claim "11KB function with class-registry behind SecuROM thunks; entity factory is the gate to spawning a second player" was wrong about what this function does. It IS dense, encrypted in places, and important — but it is the **runtime-trigger executor**, not the entity-class spawn path.

This matters because v1 of the coop plan budgeted 2–3 weeks for "RE the 11 KB factory." The actual entity factory was 310 bytes and clean. **Phase 0A's calendar cap should be lowered to 1 week.**

---

## The actual entity factory: `entity_factory_construct` at `0x5B96F0`

```c
// Renamed in IDB. Original sub_5B96F0.
// __usercall (a1<ebp>, a2<esi>, a3, a4, a5)
int entity_factory_construct(int default_name,    // ebp — default for "name" property
                              int default_misc,    // esi — default for misc properties
                              void* bag,           // a3 — a built KV bag (handle)
                              int a4, float a5)    // forwarded to sub_5B20F0 (post-init helper)
{
    // 1. Load "class" string ptr (0x6a6d74) via stolen-byte arithmetic
    //    on g_securom_thunk_table_base[+197478] - g_securom_thunk_table_base[+345350]
    const char* class_key = STOLEN_BYTE_LOAD;          // resolves to 0x6a6d74 = "class"
    const char* class_val = entity_property_get_thunk(bag, class_key, default_misc);

    // 2. Walk the registry to find the class entry whose vtable[0]() matches class_val
    void* entry = class_registry_lookup_by_name(class_val, g_class_registry_head);
    if (!entry) return 0;

    // 3. Per-class ctor: call entry->vtable[+4](this=entry, bag)
    void* entity = (*(int(__thiscall**)(void*, void*))(*(int*)entry + 4))(entry, bag);
    if (!entity) return 0;

    // 4. Apply "name" property → entity[+0x54] (== &class_metadata if absent → default_name)
    int name = entity_property_get_thunk(bag, /* "name" */ 6981780, default_name);
    if (!*(int*)((char*)entity + 84)) *(int*)((char*)entity + 84) = name;

    // 5. Look up name property on entity's own bag (entity+3196), register name→entity in scene table
    char* registered_name = entity_property_get_thunk((char*)entity + 3196, /* unknown */ 7047536, default_misc);
    sub_59D1E0(/* table */ 7129996, registered_name ? registered_name : (char*)name);

    // 6. Notify bag we're done with it (probably refcount-decrement)
    bag_merge_into(/* dst=this */ entity, bag);

    // 7. Validate
    if (!sub_55AD20(entity)) { entity->vtable[0](entity, 1); return 0; }
    sub_5B20F0(a4, a5);  // posted with a4/a5 — semantics still unclear
    if (!entity->vtable[+40](entity)) { entity->vtable[0](entity, 1); return 0; }

    // 8. Final init: register with active scene OR queue depending on global flag
    if ((dword_7193EC & entity[1]) == dword_7193E8 &&
         entity_property_get_int_thunk(entity+3196, /* unknown */ 7096512))
        sub_5AD410(entity);  // active path
    else
        sub_5AD3E0(entity);  // queued path

    *(byte*)((char*)entity + 204) = 1;  // visibility bit
    int v13 = entity_property_get_thunk((char*)entity+3196, /* unknown */ 6981768, default_misc);
    sub_55AF00(entity, v13);
    return entity;
}
```

### Callers (17)

`sub_41F040, sub_41F270, sub_41F640, sub_41F830, sub_43D167, sub_44B0D0, sub_4A7340, sub_4F3827, sub_527B30, sub_5B7C40, sub_5BB2C0, sub_5BF520, sub_5BF690, sub_5BF7D0, sub_5BF8E0, sub_5C0330, sub_5C0570`.

Spot-checked `sub_43D167` (the **wilbur slave** spawner — pushes string `"class=compActor"`):

```c
sub_4B9750(v12, /* "class=compActor" */ 0x6ADFF0);   // build bag from template
v10[0] = /* "model_name" key */ 0x6ADFE0;
sub_4B93D0("model_name");                              // set additional KV
... destroy any previous slave at *(this+960) ...
v4 = entity_factory_construct((int)v11, *(_DWORD *)(this + 4) + 88, 0.0);
*(this+960) = v4;   // slave = new entity
```

This is the canonical pattern: **build bag from string template → optionally set extra KV → call factory → store returned entity.**

The slave-spawner also illustrates that **register args (`a1`/`a2`) are caller-scope** — a1=`(int)v11` (a stack temporary holding ?), a2=`*(this+4)+88` (an offset into this's parent struct, likely the active-scene pointer). Phase 1 calls from the mod will need to determine empirically what to pass for those — initially try `0,0` and see if construction succeeds.

---

## The class registry

### Layout

A **singly-linked list** of fixed-size entries:

```c
struct ClassRegistryEntry {
    void**  vtable;   // +0x00 — 2-slot vtable {get_class_name, construct_instance}
    Entry*  next;     // +0x04 — next entry in chain (or NULL at tail)
};
```

Each vtable:
```c
struct ClassVtable {
    const char* (*get_class_name)(void* this);          // slot 0
    void*       (*construct_instance)(void* this, void* bag);  // slot +4
    // (no slot 2+ observed)
};
```

### Walker — `class_registry_lookup_by_name` at `0x5A04F0`

```c
void* class_registry_lookup_by_name(const char* class_name, void* head) {
    void* e = head;
    while (e) {
        const char* this_class = (*(const char* (**)(void*))*e)(e);  // vtable[0](this)
        if (strcmpi(class_name, this_class) == 0) return e;
        e = *(void**)((char*)e + 4);
    }
    return 0;
}
```

The strcmpi is `sub_63599D`. Match is case-insensitive, which is forgiving for template strings.

### Registration — `class_registry_link` at `0x5A0480`

```c
void class_registry_link(void* this_entry, void** head_ptr) {
    if (*head_ptr) {
        // Walk chain checking for dup; if found by class-name strcmp, do nothing
        void* e = *head_ptr;
        while (e != this_entry) {
            const char* this_name = (*(const char* (**)(void*))*this_entry)(this_entry);
            if (strcmp((*(const char* (**)(void*))*e)(e), this_name) == 0) return;
            e = *(void**)((char*)e + 4);
            if (!e) break;
        }
    }
    *(void**)((char*)this_entry + 4) = *head_ptr;  // entry->next = head
    *head_ptr = this_entry;                        // head = entry
}
```

Push-to-front. Each class self-registers via a CRT-init stub. The static-init stubs are at:

| Stub VA | Registers entry @ | Class |
|---|---|---|
| `0x6A2F40` | `0x6F57A0` | `wilbur` |
| `0x6A2F50` | `0x6F57A8` | `wilburDigDug` |
| `0x6A2F60` | `0x6F57B0` | `digDugAnt` |
| `0x6A2F70` | `0x6F57B8` | `digDugScrewball` |
| `0x6A2F80` | `0x6F57C0` | `digDugMoveable` |
| `0x6A2F90` | `0x6F57C8` | `digDugSwitch` |
| `0x6A2FA0` | `0x6F57D0` | `digDugDoor` |
| `0x6A2FB0` | `0x6F57D8` | `miniHamsterPlayer` |
| `0x6A2FC0` | `0x6F57E0` | `testPlayer` |
| `0x6A2FD0` | `0x6F57E8` | `ColorGun` |

(11 stubs at 16-byte spacing; 6 more classes register from larger init routines elsewhere.)

Each stub is the same shape:
```asm
push    offset g_class_registry_head
mov     ecx, offset class_entry
call    class_registry_link
retn
```

### Full registry contents — 17 classes

Walked at this snapshot from `g_class_registry_head = 0x705454` via `next` links. Most-recently-registered first:

| # | Entry VA | Vtable VA | get_class_name fn | **Class** |
|---|---|---|---|---|
| 1 (head) | `0x705454` | `0x6CCAAC` | `0x5B71B0 classname_protagonist` | **`protagonist`** ← player |
| 2 | `0x705430` | `0x6CBE60` | `0x5B1710 classname_actor` | `actor` |
| 3 | `0x7053F4` | `0x6CBA04` | `0x5AD8C0 classname_compActor` | `compActor` |
| 4 | `0x6FBBAC` | `0x6C6A94` | `0x553920 classname_plant` | `plant` |
| 5 | `0x6FBB90` | `0x6C679C` | `0x552860` (stolen-byte) | `triggerbox` |
| 6 | `0x6F6458` | `0x6BFE08` | `0x51F150 classname_playerActor` | `playerActor` |
| 7 | `0x6F57E8` | `0x6B8600` | `0x48D5D0` (stolen-byte) | `ColorGun` |
| 8 | `0x6F57E0` | `0x6B85EC` | `0x48D5C0` (stolen-byte) | `testPlayer` |
| 9 | `0x6F57D8` | `0x6B85D0` | `0x48D5B0` (stolen-byte) | `miniHamsterPlayer` |
| 10 | `0x6F57D0` | `0x6B85BC` | `0x48D5A0` (stolen-byte) | `digDugDoor` |
| 11 | `0x6F57C8` | `0x6B85A4` | `0x48D590` (stolen-byte) | `digDugSwitch` |
| 12 | `0x6F57C0` | `0x6B858C` | `0x48D580` (stolen-byte) | `digDugMoveable` |
| 13 | `0x6F57B8` | `0x6B8574` | `0x48D570` (stolen-byte) | `digDugScrewball` |
| 14 | `0x6F57B0` | `0x6B8560` | `0x48D560` (stolen-byte) | `digDugAnt` |
| 15 | `0x6F57A8` | `0x6B8548` | `0x48D550` (stolen-byte) | `wilburDigDug` |
| 16 | `0x6F57A0` | `0x6B8614` | `0x48D5E0` (stolen-byte) | `wilbur` |
| 17 (tail) | `0x6F3784` | `0x6A742C` | `0x40C140 classname_digdug` | `digdug` |

The 11 "stolen-byte" `0x48D5XX` accessors all share the same opcode pattern (`dd 0E9FFF766h, 0FFF78631h, …; jmp ds:[g_securom_thunk_table_base+0x2F506]`). Statically opaque, but at runtime each returns the inline string sitting next to its vtable in `.rdata` — which we extracted via byte inspection of the surrounding bytes.

The recon doc's "50+ entity classes registered at boot" claim was **wrong**. There are 17. The 50+ figure was probably counting either property keys or scripted-command verbs.

### Player-related classes

Three classes are player-controllable variants:

- **`protagonist`** — main game's player class. **THIS is the gate to coop.**
- `playerActor` — likely older / generic player-actor base class. Worth checking if the `protagonist` C++ class extends `playerActor` (vtable inspection will reveal at Phase 2).
- `wilbur` — DigDug-style mini-game variant.
- `wilburDigDug`, `miniHamsterPlayer`, `testPlayer` — other mini-game variants.

`protagonist`'s `.rdata` neighborhood includes property keys: `damageSpecial`, `damageExplosive` — confirms the class has damage-typed defenses, consistent with the player avatar.

---

## Bag KV-store internals

### Storage is hashed, not stringly-typed

`bag_merge_into` (formerly `sub_4B95A0`) is decompilable and reveals the bag layout:

```c
void* bag_merge_into(BagHead* dst, BagHead* src) {
    SrcNode* sn = *src;
    while (sn) {
        const char* key   = sn->key_str;
        const char* val   = sn->val_str;
        uint32_t key_id   = string_intern_hash(key);   // sub_5D3E30
        uint32_t val_id   = string_intern_hash(val);

        // Find existing entry with same key_id in dst
        DstNode* d = *dst;
        while (d && d->key_id != key_id) d = d->next;

        if (!d) {
            // Allocate new node from freelist (init via sub_4B92B0 if needed)
            d = freelist_alloc(&unk_724B80);
            d->key_id = key_id;
            d->next = *dst;
            *dst = d;
        }
        d->val_id = val_id;

        sn = sn->next;
    }
    return d;
}
```

So the bag is internally a `head_ptr → linked list of {next, key_id, val_id} nodes` where keys and values are **string-intern hash IDs**. `string_intern_hash` (`sub_5D3E30`, 28+ callers) is the canonical interner; given the same string, it returns the same uint32 ID across the whole engine.

This means:
- Two bags built from the same template will have identical `key_id`s for matching keys — independent of where the strings live in memory.
- `entity_property_get_thunk(bag, "class")` works by hashing `"class"` and walking the bag's hash chain.
- Setting a property requires a hash-id, not the string. The thunked `bag_set_kv` is the only way; we can't bypass it.

### Bag construction — `bag_init_from_template` (THUNK, runtime-only)

`sub_4B9750` is a single instruction: `jmp ds:[g_securom_thunk_table_base + 0x3307A]`. The slot at `0xF928F0` resolves at runtime to **VA `0x1007700`** — outside the main image (probably an unpacked auxiliary segment in the original SecuROM build). The IDB has no function definition there.

**This is fine for Phase 1+.** The mod can call `bag_init_from_template_THUNK` like any other engine function — the runtime indirection table is initialized at process startup, so the `jmp` works. We have empirical evidence: the existing `entity_kv` wrapper does the exact same thing for `entity_property_get_thunk` (also a stolen-byte thunk, also resolves to a runtime VA).

### Bag empty/destroy

The decompile shows `bag_merge_into(entity, bag)` called at the END of `entity_factory_construct`. That call signature `(this=entity, src=bag)` MERGES the temporary bag into the entity's permanent bag at `entity+3196`. So:

- The temporary bag built by the caller (e.g. on stack) is consumed by the factory.
- After factory returns, the caller does NOT need to explicitly destroy the bag — its contents have been merged into the entity. The stack-allocated handle goes out of scope normally.

This simplifies the caller pattern.

---

## What this unblocks for Phase 1+

### Phase 1 (UDP transport) — unaffected

Phase 1 has no entity-system dependency. Proceed independently.

### Phase 2 (second-player spawn) — design-locked now

With the factory characterized, the Phase 2 design is:

1. **Spawn the second protagonist:**
   ```cpp
   void* bag = nullptr;
   bag_init_from_template_THUNK(&bag, "class=protagonist; name=player2; …");
   void* p2 = entity_factory_construct(0, 0, &bag, 0, 0.0f);
   if (!p2) { /* registry lookup or ctor failed */ }
   ```
   Initial register-arg defaults are 0/0; verify empirically.

2. **Bind it to input source 1** (the orphan `g_input_mgr` slot) — Phase 2's other half. `g_input_mgr` IS still a trap per the audit's gotcha, so this needs the full input-routing rework, but the entity-side is unblocked.

3. **Test scenario `coop-second-player-spawn`:** mod boots single-player, presses a hotkey that triggers the factory call from the mod, asserts a new entity at `dword_724DE4`+1 (transform list grows by 1), asserts the new entity's name property = `player2`.

### Pre-Phase-1 derisking experiment — try the factory call NOW

Cheap and high-information: spawn a second `protagonist` from the mod (no networking, no input bind, just observe). If the factory returns non-null and the engine doesn't crash on the next sim tick, the entire entity-factory path is validated. If it crashes, we've found the missing dependency before touching networking.

This is a Phase 1.0 task ("validate the factory contract") that should be done before committing to Phase 2's full input-routing work.

### Save-system RE (Phase 0B) — still pending

Unchanged scope — the save format is wholly uncharted. Estimate stands at ~1 week.

### .sx command catalog (Phase 0C) — partial bonus

While reorienting on `sub_5B40C0` we observed enough strings to inventory the `commandType` verbs (~50 visible in the function's first quarter). Phase 0C (full .sx command catalog) can be partially populated by walking `script_command_dispatch_giant`'s string references rather than purely from `Game/data/scripts/*.sx` text scanning. Estimate: ~3 days, not 1 week.

---

## IDB renames + comments shipped

| Old | New | Address |
|---|---|---|
| `sub_5B96F0` | `entity_factory_construct` | `0x5B96F0` |
| `sub_5B96A0` | `entity_factory_lookup_and_construct` | `0x5B96A0` |
| `sub_5A04F0` | `class_registry_lookup_by_name` | `0x5A04F0` |
| `sub_5A0480` | `class_registry_link` | `0x5A0480` |
| `dword_7429C8` | `g_class_registry_head` | `0x7429C8` |
| `sub_4B9750` | `bag_init_from_template_THUNK` | `0x4B9750` |
| `sub_4B93D0` | `bag_set_kv_THUNK` | `0x4B93D0` |
| `sub_4B95A0` | `bag_merge_into` | `0x4B95A0` |
| `sub_5B71B0` | `classname_protagonist` | `0x5B71B0` |
| `sub_5B1710` | `classname_actor` | `0x5B1710` |
| `sub_5AD8C0` | `classname_compActor` | `0x5AD8C0` |
| `sub_553920` | `classname_plant` | `0x553920` |
| `sub_51F150` | `classname_playerActor` | `0x51F150` |
| `sub_40C140` | `classname_digdug` | `0x40C140` |

`0x552860` (`triggerbox` accessor) failed rename — IDA reports "Function not found" because it sits in stolen-byte code that hasn't been auto-defined as a function. Functional effect: none; the comment metadata at `0x5B96F0`, `0x5A04F0`, `0x5A0480`, `0x7429C8`, `0x4B95A0` documents the registry + bag architecture.

---

## Open questions for Phase 0B+

1. **Register args `a1`/`a2` (ebp/esi) of `entity_factory_construct`** — observable empirically by tracing 2 callers in Phase 1.0; static analysis will probably never resolve cleanly.
2. **Layout of the bag handle** — is it a single dword (head ptr) or a small struct? Looking at `sub_43D167` it appears to be a single dword. Confirmable by inspecting `bag_init_from_template_THUNK`'s prologue at runtime VA `0x1007700` (e.g. patching a hardware breakpoint and reading args).
3. **Property sets needed for a minimal `protagonist`** — when a protagonist template lacks a key like `model_name`, does the factory fault, or does the per-class ctor have defaults? Test: build a bag with just `class=protagonist; name=player2;` and call the factory; observe.
4. **Lifetime ownership** — once the factory returns, who owns the entity? Does it auto-link into all the engine's per-frame walkers (anim, transform list, AI), or are extra registrations needed? `sub_5AD410` ("active path") in the factory's tail looks like that registration; verify Phase 1.0.
5. **`sub_5B96A0`** is `entity_factory_lookup_and_construct` — same lookup + construct pattern but skips name registration and post-init. May be useful for "construct without scene insertion" mode (e.g. for a hidden P2 placeholder before the network handshake completes).

---

## Files

- IDB updated: see rename table above.
- Plan: [coop-multiplayer-plan-2026-05-10.md](coop-multiplayer-plan-2026-05-10.md).
- Predecessor docs: [entity-system.md](entity-system.md) (now partially superseded by this doc on the factory point), [player-entity-layout-2026-05-09.md](player-entity-layout-2026-05-09.md).

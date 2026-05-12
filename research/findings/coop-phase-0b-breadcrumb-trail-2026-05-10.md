# Coop Phase 0B — Factory crash localized via breadcrumb ladder (2026-05-10)

**Status:** GREEN forward progress. Took the autonomous loop from `bc=[0,0,0,0,0,0]` (factory crashes at entry) to `bc=[reg=1,c_pre=1,c_post=1,merge=1,v1_pre=1,v1_post=1,tx_pre=1,tx_post=1,a=0,q=0,pi=0]` (factory reaches `vtable[10] = sub_5B7010` — an actor post-init step). The factory's gated dependency chain is now mapped; remaining work is inside the post-init's per-class behavior.
**Scenario:** `pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy` — 9-12s per iteration.
**Build:** mtr-asi.asi = 664,576 bytes deployed.

## What ships

The probe at [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp) now installs a 7-hook ladder during `try_spawn_p2`:

| Order | Address | Function | Role |
|---|---|---|---|
| bc[0] | 0x5A04F0 | `class_registry_lookup_by_name` | PRE-only — proves bag accessor returned class string |
| bc[1] | 0x5B71C0 | `protagonist_ctor` (vtable[+4]) | PRE — proves registry dispatch worked |
| bc[2] | 0x5B71C0 | (POST-return) | proves ctor body returned |
| bc[3] | 0x4B95A0 | `bag_merge_into` | PRE — proves we reached the merge |
| bc[3.5] | 0x55AD20 | `sub_55AD20` (validate1) | PRE — proves we reached validate1 |
| bc[3.6] | 0x55AD20 | (POST + result) | captures return value |
| bc[3.7] | 0x5B20F0 | `sub_5B20F0` (transform setup) | PRE — proves we reached transform |
| bc[3.8] | 0x5B20F0 | (POST + result) | captures result |
| bc[4a/4b] | 0x5AD410 / 0x5AD3E0 | active/queued register | PRE — proves we reached scene insertion |
| bc[5] | 0x55AF00 | `sub_55AF00` (post-init) | PRE — proves we reached the very tail |

All hooks scope via a single `g_observing` flag — engine's normal entity-construction paths see unhooked behavior.

## Five substantive audit corrections found this session

### Correction 1 — Factory signature: a4 is `init_pos_vec3*`, NOT `default_misc`

The Phase 0A audit annotated `entity_factory_construct` as taking `(bag, default_misc, a5)` based on IDA's `__usercall` decoration. **That was wrong.** `a4` is dereferenced as a 3-float position vector by `sub_5B20F0` (transform setup at factory step 9):

```c
*(this + 22) = *a2;       // entity+0x58 = pos.x
*(this + 23) = a2[1];     // entity+0x5C = pos.y
*(this + 24) = a2[2];     // entity+0x60 = pos.z
```

Passing `0` (NULL) crashes `sub_5B20F0` on the first `*a2` deref. Empirically verified: bc[3.7]/[3.8] never fired with NULL; both fire after passing a valid `&float[3]`.

`a5` is an initial rotation angle in radians (sin/cos used to fill the rot matrix at entity+0x70).

**New signature:**
```cpp
using PFN_Factory = void* (__cdecl*)(void* bag_descriptor,
                                     const float* init_pos_vec3,
                                     float init_rot_radians);
```

### Correction 2 — Bag descriptor layout is single address, NOT 2-slot or 12-byte

The Phase 0A audit claimed:
> sub_43D167 ... uses TWO stack slots for the bag descriptor:
>   - bag_init_from_template_THUNK is called with ECX = &slots[1]
>   - entity_factory_construct is called with first stack arg = &slots[0]

This was an IDA naming artifact. The IDA frame metadata for `sub_43D167` shows `var_18` at offset 0x14 and `var_10` at offset 0x1C — superficially 8 bytes apart, but the disassembly's `[esp+depth+var_X]` resolution for both lea instructions arrives at the **same memory address**. IDA labels the same dword as both `var_10` and `var_18` depending on the depth at the access site.

**The truth, confirmed by `bag_merge_into` decompile (sub_4B95A0):**
```c
int **v3 = (int **)*a2;  // head pointer at offset +0
```

The bag handle is a single `void**` — head pointer at offset +0. `bag_init_from_template` and `factory` both receive the SAME address.

**Working layout:**
```cpp
void* slots[4] = { nullptr, nullptr, nullptr, nullptr };  // overallocate for safety
bag_init(&slots[0], nullptr, "class=protagonist");
bag_set(&slots[0], nullptr, "name", "player2");
factory(&slots[0], init_pos, 0.0f);
```

### Correction 3 — `bag_init_from_template` is single-KV only

The audit's example template was `"class=protagonist; name=player2; …"` — multi-KV with semicolons. **The parser doesn't handle this.** The engine's actual usage in `sub_43D167`:

```c
bag_init_from_template_THUNK(&bag, "class=compActor");      // single KV
bag_set_kv_THUNK(&bag, "model_name", "objects/protectosphere"); // additional KVs go here
```

Multi-KV templates leave the bag empty (or partially populated — exact behavior unverified). Empirically: `"class=protagonist; name=player2;"` produced an empty-for-"class" bag, causing `entity_property_get_thunk(bag, "class")` to return NULL → `class_registry_lookup_by_name(NULL, head)` → strcmpi NULL crash.

**Use single-KV template + chained `bag_set_kv` calls.**

### Correction 4 — `sub_55AD20` (validate1) fails for headless entities

The factory's first post-bag-merge gate is `sub_55AD20(entity)`. Per its decompile, it returns 1 if any of three sub-validations pass:
- `sub_55AC90(entity)` — SecuROM thunk → runtime address
- `unk_729748(entity)` — global function ptr; **always NULL at runtime** (this branch is dead in this build)
- `sub_55AC40(entity)` — SecuROM thunk → runtime address

For our headless protagonist, all three return false → factory destroys + returns NULL.

What state these check is unknown statically (both are SecuROM-resolved). Path B (entity[+0x4C] & 4) was ruled out empirically — bit 2 was 0.

**Diagnostic bypass:** the probe currently force-passes validate1 in observing-scope to keep the breadcrumb trail moving. This is a temporary measure; understanding what the validators check is part of Phase 0C.

### Correction 5 — vtable[10] = `sub_5B7010` is `actor::activate()`

After validate1 + transform_setup, the factory calls `vtable[10]` on the entity. For protagonist this resolves to **`sub_5B7010`** (NOT a SecuROM thunk — fully decompilable):

```c
char sub_5B7010(int this) {
    char result = sub_5B1E10(this);   // actor post-init: model loads, anim loads, script reg
    if (result) {
        if (!*(_DWORD *)(this + 3244)) {
            int v3 = sub_5832C0(152);      // alloc 152 bytes
            int v4 = v3 ? sub_5794D0(v3) : 0;
            sub_5B6E80(v4, 0, 1);          // input system hookup?
            sub_579BF0(-1, 0.0);            // device binding (-1 likely "default keyboard")
        }
        *(_DWORD *)(this + 3240) = dword_7426C4;
        return 1;
    }
    return result;
}
```

This IS where the protagonist gets bound to the input system — `sub_579BF0(-1, 0.0)` looks like input device binding (the v2-coop plan's gotcha 6: `g_input_mgr` is a trap, but THIS path may be the legitimate input bind).

`sub_5B1E10` (called first) is the heavy actor post-init: reads `mdb1..mdb4` and `anim1..anim4` from the bag, calls vtable[44] and vtable[27], registers a script command. **THIS is where our current iteration crashes** — bc[3.8] fires (transform_setup completes) but bc[4a]/[4b] never fires.

## Current diagnostic capture

```
[20:14:57.729] coop_spawn_probe: bc[0] sub_5A04F0 PRE: class_name=0EFDA730 ("protagonist") head=00705454
[20:14:57.729] coop_spawn_probe: bc[1] sub_5B71C0 PRE: this=00705454 bag=030FEB60
[20:14:57.729] coop_spawn_probe: bc[3.7] sub_5B20F0 PRE: entity=0EFD9A20 pos=030FE8B8 (0,0,0) rot=0  ← INSIDE ctor body
[20:14:57.729] coop_spawn_probe: bc[3.8] sub_5B20F0 POST: result=0
[20:14:57.729] coop_spawn_probe: bc[2] sub_5B71C0 POST: returned=0EFD9A20
[20:14:57.729] coop_spawn_probe: bc[3] sub_4B95A0 PRE: dst=0EFDA69C src=030FEB60
[20:14:57.729] coop_spawn_probe: bc[3.5] sub_55AD20 PRE: entity=0EFD9A20 entity+0x4C=0 (bit2=0) entity+4=0x90000000
[20:14:57.729] coop_spawn_probe: bc[3.6] sub_55AD20 POST result=0 (force-passing)
[20:14:57.729] coop_spawn_probe: bc[3.7] sub_5B20F0 PRE: entity=0EFD9A20 pos=549BEE44 (0,0,0) rot=0  ← OUR call (factory step 9)
[20:14:57.729] coop_spawn_probe: bc[3.8] sub_5B20F0 POST: result=0
[20:14:58.705] coop_spawn_probe: EXCEPTION (bc=[reg=1,c_pre=1,c_post=1,merge=1,v1_pre=1,v1_post=1,v1_res=0,tx_pre=1,tx_post=1,a=0,q=0,pi=0])
```

Notice `sub_5B20F0` is called **twice**: once during the protagonist ctor body (sub_5B6F40 calls it internally to init the entity's transform), and once at the factory's step 9. Both succeed.

The crash is between bc[3.8] (transform POST) and the factory's vtable[10] dispatch (which would precede a register_active/queued call). So the crash is INSIDE `sub_5B7010` (vtable[10]) → almost certainly inside `sub_5B1E10` (the actor post-init).

## Next-session candidates

1. **Phase 0C-step-1 (~30 min)**: Add a PRE+POST hook on `sub_5B1E10`. If POST fires, the crash is in `sub_5B7010`'s remaining steps (alloc, sub_5794D0, etc.). If POST doesn't fire, deeper-instrument inside `sub_5B1E10`.
2. **Phase 0C-step-2 (~1-2h)**: Identify which sub-call inside `sub_5B1E10` faults. Top candidates: vtable[44] (`*(this+176)`), vtable[27] (`*(this+108)`), `script_register_command` with garbage entity+56 handle.
3. **Phase 0C-step-3 (~2-4h)**: Once isolated, decide: is it a missing bag key (e.g., `mdb1` for model), a missing scene context, or a fundamental "headless construction unsupported" wall?
4. **Phase 0D — investigate validate1 SecuROM thunks**: Resolve `sub_55AC90` and `sub_55AC40` real addresses at runtime via the IAT slots (same trick as `sub_55AF00` resolution). Then disassemble to understand what they check. This removes the need for the force-pass diagnostic bypass.

## What this session demonstrates about Phase 1+ feasibility

- ✅ The factory CAN be called from outside the engine via the slave-spawner pattern.
- ✅ Bag construction works correctly with single-KV template + chained `bag_set_kv`.
- ✅ Registry lookup, per-class ctor, and bag-merge all complete cleanly.
- ✅ The entity is fully allocated (3276 bytes) and base-initialized (transform list registered, bag at +3196 ready, actor base ctor complete).
- ⚠ Two engine-state checks remain (validate1's 3 sub-validators + actor post-init) before the entity is "live."

The cheap-derisk experiment from the audit succeeded in proving the factory contract works for at least the first 10 of 12 steps. **The remaining 2 steps both relate to scene-state setup the engine normally inherits from `.sx` script context.** If those checks turn out to be cosmetic (e.g., "is in scene's active list" — addressable by manually adding to the list before the call), Phase 1's coop-spawn path is essentially unblocked. If they turn out to require deep `.sx` context replay (e.g., scene-load completion, controller binding, save-state restore), the v2-plan's Phase 2 timeline holds.

## Files modified this session

- [src/mtr-asi/include/mtr/coop_spawn_probe.h](../../src/mtr-asi/include/mtr/coop_spawn_probe.h) — added breadcrumb fields (registry_pre, ctor_pre/post, merge_pre, validate1_pre/post/result, transform_pre/post)
- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp) — extended hook ladder to 7 hooks + force-pass validate1 diagnostic
- [src/mtr-asi/src/menu/tab_debug.cpp](../../src/mtr-asi/src/menu/tab_debug.cpp) — display 3-slot bag descriptor

## Engine VAs newly characterized

| VA | Symbol | Notes |
|---|---|---|
| 0x5A04F0 | `class_registry_lookup_by_name` | __cdecl(class_name, head) |
| 0x5B71C0 | `protagonist_ctor` (vtable[+4]) | __thiscall(this=registry_entry, bag) — body in sub_5B6F40 |
| 0x4B95A0 | `bag_merge_into` | __thiscall(dst, src). Head ptr at offset +0 |
| 0x55AD20 | `entity_validate1` (sub_55AD20) | __cdecl(entity). 3 sub-validators OR'd, fails for headless |
| 0x5B20F0 | `entity_transform_setup` (sub_5B20F0) | __thiscall(entity, pos_vec3, rot_radians). Sets entity+0x58 + entity+0x70 |
| 0x5B7010 | `protagonist_vtable[10]` | __thiscall(entity). Calls sub_5B1E10 + input bind chain |
| 0x5B1E10 | `actor_post_init` | __thiscall(entity). Reads mdb1..4/anim1..4 from bag; calls vtable[44]+[27]; **CURRENT FAULT SITE** |
| 0x6CC9A8 | protagonist entity vtable | NOT 0x6CCAAC (that's the registry entry's vtable) |

---

# Phase 0C-step-1 result (2026-05-10, second autonomous loop)

**Status:** GREEN forward progress. Crash fully localized to **inside `sub_5B1E10`** (NOT later in `sub_5B7010`). Smoking-gun cause captured.

**Build:** mtr-asi.asi = 665,600 bytes deployed. Test elapsed 10,359 ms / 1873 frames.

## What ships

Added an 8th hook to the breadcrumb ladder:

| Order | Address | Function | Role |
|---|---|---|---|
| bc[3.9] | 0x5B1E10 | `sub_5B1E10` (actor_init) | PRE — proves we entered actor post-init |
| bc[3.95] | 0x5B1E10 | (POST + result) | proves actor post-init returned |

The PRE hook also captures `entity+0x1EC` (= `*((DWORD*)this + 123)` — the entity's bag chain head pointer that `sub_5B1E10` reads as its very first instruction). Capturing it inline in the PRE log gives a one-shot data snapshot of state at the actor-init boundary without needing a separate IDA reattach.

## Crash localization

```
[21:29:40.099] bc[3.7] sub_5B20F0 PRE: entity=0EFDCA20 pos=537CEEFC (0,0,0) rot=0  ← factory step 9
[21:29:40.099] bc[3.8] sub_5B20F0 POST: result=0x0
[21:29:40.099] bc[3.9] sub_5B1E10 PRE: entity=0EFDCA20 entity+0x1EC(bag_chain_head)=0x00000000  ← !!!
[21:29:40.984] EXCEPTION  bc=[..., ai_pre=1, ai_post=0, ai_res=-1, ...]
```

**Two new facts:**

1. **`ai_pre=1, ai_post=0`** — the crash is INSIDE `sub_5B1E10`'s body, NOT later in `sub_5B7010` (the alloc + `sub_5794D0` + `sub_5B6E80` + `sub_579BF0` chain after `sub_5B1E10` returns). That chain is therefore innocent for now.

2. **`entity+0x1EC = 0x00000000`** — the entity's primary bag chain head pointer is **NULL** at the moment we enter actor_init. This is the entity's _first_ bag descriptor (NOT the +3196 bag that `bag_merge_into` populated — `merge_pre`'s log line shows `dst=0EFDD69C = entity+0xC7C = +3196`, which is the second bag).

3. **884 ms gap between PRE and exception.** That's a long time for a single-threaded fault. Suggests either:
    - Deep call stack (vtable[44] dispatch, 2640-byte memset, script_register_command, mdb1..4 + anim1..4 entity_property_get_thunk loop, etc.) running heavy work before tripping
    - Unhandled SEH bouncing through the engine's panic handler chain before our outer `__except` catches it
    - Or a wait inside `entity_property_get_thunk` querying a NULL bag, returning, then crashing on the result deref

## Decompile of the suspect path

`sub_5B1E10` reads `*((DWORD*)this+123)` (= `entity+0x1EC`) as its first action:

```c
char sub_5B1E10(float *this) {
    int v2 = *((DWORD *)this + 123);   // = entity+0x1EC = our captured 0x00000000
    DWORD *v3 = this + 123;
    if (v2) {                          // SKIPPED (v2=0)
        int v4 = *(DWORD *)(v2 + 164);
        if (v4) sub_4CD7B0(v4, 20);
    }
    *((DWORD *)this + 63) = sub_5BBD10(*v3);   // *v3 = NULL → sub_5BBD10(NULL)
    (*(void(__thiscall **)(float *))(*(DWORD *)this + 176))(this);  // vtable[44]
    memset(this + 127, 0, 0xA50u);     // 2640-byte zero
    // ... [snip] ...
    do {
        sub_62A6E6(v18, "mdb%d", v8 + 1);
        v10 = entity_property_get_thunk(v18);   // queries entity's PRIMARY bag → NULL chain → ?
        if (v10) sub_4EB6A0(v10);                // model load
        ++v8;
    } while (v9 < 4);
    // ... same loop for anim1..4 ...
}
```

**Two top-candidate fault sites in the body:**

- **`sub_5BBD10(NULL)`** — single ptr-arg call with our NULL bag chain head. Decompile of `sub_5BBD10` would tell us if it null-checks. If it doesn't, this is the immediate fault. (Cheap to verify: read the disasm.)
- **`(*(this+176))(this)`** — vtable[44] dispatch on our newly-allocated 3276-byte entity. The vtable IS populated (ctor wrote it; the registry walk found the right class), but vtable[44] for `protagonist` may itself read the (now-zeroed) bag chain or some other not-yet-set field.
- **`entity_property_get_thunk` against a chain-of-bags walker that requires entity+0x1EC chain** — if the thunk walks the bag chain and our chain head is NULL, the walker may crash on first deref.

## Why is `entity+0x1EC` NULL?

Per the user's existing audit (project_coop_phase_0a_audit_shipped) the entity has TWO bag descriptors:

- `+0x1EC` (= +492) — the **primary** entity bag (the chain of inherited class kv-bags built up by .sx-script load). **Our path never populates this.** The factory sets `+0xC7C` (the secondary, per-instance bag) via `bag_merge_into` from the caller's bag, but the primary chain comes from the engine's class-load pipeline that we bypass.
- `+0xC7C` (= +3196) — the per-instance bag, populated by `bag_merge_into`.

So we're hitting the predicted-but-not-yet-confirmed wall: **the actor post-init reads the primary bag chain, which is normally seeded by the .sx class-load pipeline that runs at level-load time, NOT the per-spawn factory call.**

This is a strong negative finding for the "cheap factory call" approach: the factory contract assumes the primary bag chain is already populated by the time `vtable[10]` runs. To call the factory standalone we'd need to either:

1. **Synthesize the primary bag chain ourselves before the factory call** — walk the registry entry to find the canonical class-bag for "protagonist" and stitch it into `entity+0x1EC` between ctor return and vtable[10] dispatch. Possibly a 1-line patch if we can find the registry's bag pointer.
2. **Call the engine's class-load pipeline first** — find the function that `.sx` script-load uses to init the primary chain and call it on our entity before the factory.
3. **Patch sub_5B1E10 to no-op the primary-bag-dependent code** — diagnostic only, would result in a "spawned but inert" entity; not useful for coop but useful for further breadcrumb localization (rule out vtable[44] vs entity_property_get_thunk).

## Next-session candidates

1. **Phase 0C-step-2a (~1-2h)**: Disassemble `sub_5BBD10` to see if the NULL primary bag deref is the immediate fault (cheap derisk).
2. **Phase 0C-step-2b (~2-4h)**: Find the engine's primary-bag-chain seeder by xrefs-to entity+0x1EC writes — see if there's a single function that runs at class-load to populate the chain.
3. **Phase 0C-step-2c (~30min)**: Hook `entity_property_get_thunk` PRE in observing-scope to see _which_ key triggers the fault (mdb1, anim2, etc.). Cheap.
4. **Phase 0E — Headless-spawn alternate path**: Instead of replicating the factory, look for a "clone existing player" entry point. The coop pattern we want (P1's load-game state cloned as P2 with a new input device) may be addressable via a much simpler `entity_clone(player, input_device=2)` if such a path exists. Worth a 1-hour search before sinking another 4 hours into factory-replication.

## What this session demonstrates about Phase 1+ feasibility

- ✅ Crash is now localized to a 1-function interval (sub_5B1E10), with the proximate state captured (entity+0x1EC = NULL).
- ✅ The fault is a **state precondition** issue (missing primary bag chain), NOT a corrupted-pointer or invalid-this issue. That's a much more tractable problem.
- ⚠ The factory contract requires the primary bag chain to be set up by the engine's class-load pipeline. **The "cheap factory call" approach probably needs one extra step: bag-chain seeding.** That step's cost is unknown until step-2b.

## Files modified this session

- [src/mtr-asi/include/mtr/coop_spawn_probe.h](../../src/mtr-asi/include/mtr/coop_spawn_probe.h) — added `actor_init_pre` + `actor_init_post` fields
- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp) — added 8th hook (`hk_actor_init`) + extended bc string + +0x1EC capture in PRE log

## Engine VAs newly characterized this step

| VA | Symbol | Notes |
|---|---|---|
| 0x5BBD10 | `<unknown>` (called from sub_5B1E10) | Single-arg fn called with `entity+0x1EC` (the bag chain head). Untouched by audit; needs disasm. |
| `entity+0x1EC` | primary bag chain head pointer | The entity's class-inherited bag chain. NOT populated by factory path — needs separate seeding from `.sx` class-load pipeline. |

---

# Phase 0C-step-2a result (2026-05-10, third autonomous loop)

**Status:** GREEN forward progress (NEGATIVE result with strong decision value). Tested the "single-point bypass" hypothesis. **Disproven.** The bag-chain dependency is structural, not localized to one fault site.

**Build:** mtr-asi.asi = 666,624 bytes deployed. Test elapsed 10,234 ms / 1873 frames.

## Hypothesis tested

If `sub_5BBD10(NULL)` was the ONLY NULL-deref of `entity+0x1EC` in `sub_5B1E10`, then a probe-scoped short-circuit (return 0 when arg is NULL) should let `sub_5B1E10` complete and `ai_post=1` would fire on next iteration.

## Implementation

Added a 9th hook (`hk_bbd10` at 0x5BBD10) to the breadcrumb ladder. Hook scope: probe-window (`g_observing`). Behavior: if `arg == NULL`, log + short-circuit return 0 (mirrors the function's own three "return 0" branches at the top of its body, on its own null branches). Otherwise call original.

## Result

```
[21:50:27.563] bc[3.9]   sub_5B1E10 PRE: entity=0EEBBA20 entity+0x1EC=0x00000000
[21:50:27.563] bc[3.91]  sub_5BBD10 PRE arg=NULL — SHORT-CIRCUITING (return 0; bypasses *(NULL+164) deref crash)
[21:50:28.575] EXCEPTION  bc=[..., ai_pre=1, ai_post=0, bbd10_pre=1, bbd10_byp=1, ...]
```

`bbd10_byp=1` confirms the short-circuit fired. `ai_post=0` confirms `sub_5B1E10` STILL crashed before returning. So `sub_5BBD10` is NOT the only fault site — there's at least one more NULL-deref of `entity+0x1EC` (or a downstream consequence) inside `sub_5B1E10`.

## Why bypass alone fails — the second fault site in sub_5B1E10

Reading the body more carefully:

```c
char sub_5B1E10(float *this) {
    int v2 = *((DWORD *)this + 123);    // entity+0x1EC = NULL
    DWORD *v3 = this + 123;             // v3 points to entity+0x1EC
    if (v2) { ... }                      // skipped (v2=0)
    *((DWORD *)this + 63) = sub_5BBD10(*v3);   // ← FIRST FAULT (now bypassed)
    (*(this+176))(this);                 // vtable[44] dispatch
    memset(this + 127, 0, 0xA50u);       // 2640-byte zero
    // 10-iteration write loop...
    int v7 = *v3;                        // ← v7 = NULL again
    *(this + 109) = 0.0;
    *(this + 110) = *(float *)(v7 + 172);   // ← SECOND FAULT: *(NULL+172) = *0xAC → AV
    sub_5B0FE0(this);
    // ... entity_property_get_thunk("mdb%d") loop ...
    // ... entity_property_get_thunk("anim%d") loop ...
}
```

So there are at minimum TWO NULL-derefs of `entity+0x1EC` in `sub_5B1E10`, plus indirect dependencies via `(*(this+176))(this)` (vtable[44]) and the property-get loops.

The 1012ms gap between bbd10 PRE-log and exception (21:50:27.563 → 21:50:28.575) is consistent with: short-circuit returns instantly, vtable[44] runs (possibly heavy work), memset+loop run, then the second `*(v7+172)` deref faults. So vtable[44] likely succeeded (didn't read entity+0x1EC), and the second `*v3` deref is the new fault site.

## Why the engine's normal path doesn't crash here

This is the puzzle. We confirmed:

1. **The ctor wrapper `sub_5B71C0` IGNORES its bag arg.** Disassembly:
   ```
   push 0CCCh; call sub_5832C0    ; alloc 3276 bytes
   mov ecx, eax; call sub_5B6F40   ; ctor body
   ret 4                            ; bag arg discarded
   ```
   So whatever bag the factory passes to the ctor is dropped on the floor.

2. **The ctor body explicitly zeros `entity+0x1EC`.** Inside `sub_5B6F40` → `sub_5B3F10` → `sub_5B37C0`:
   ```c
   *(_DWORD *)(this + 492) = 0;   // entity+0x1EC = NULL
   ```
   So after the entire ctor chain, `entity+0x1EC` is GUARANTEED NULL.

3. **The factory body has no writes to `entity+0x1EC`** between ctor return and `vtable[10]` dispatch. The only operations are:
   - `entity_property_get_thunk(a3, ..., a1)` — read from caller's bag
   - Conditional write to `entity+84` (NOT +0x1EC)
   - Read+strcmp of a key from `entity+3196` (empty per-instance bag at this point)
   - `bag_merge_into(entity+3196, a3)` — populates per-instance bag
   - `sub_55AD20(entity)` — validate1
   - `sub_5B20F0(entity, a4, a5)` — transform_setup
   - **`vtable[10](entity)` ← reads entity+0x1EC = NULL → fault**

So **per the static analysis, the engine's normal path SHOULD ALSO crash** here for any class whose `vtable[10]` reads `entity+0x1EC`. But it doesn't. Three explanations:

1. **Theory A — `entity_factory_construct` is never called for protagonist in normal play.** The 17 registered classes might all be `.sx`-script-spawned auxiliaries (NPCs, props, particles), and the player is allocated via a different path entirely. **Most likely.** The `protagonist` class is registered as a target of the `.sx` `create` command, but the engine's level-load pipeline might use a separate "primary actor" path that takes a fully-prepared bag chain.

2. **Theory B — vtable[10] is conditionally skipped via the `dword_7193EC & *(v9 + 4) == dword_7193E8` flag.** The factory has a branch right after vtable[10] that reads entity+4 and gates active vs queued register. Maybe in the normal path, vtable[10] never returns into this code because it's intercepted somewhere. (Less likely — the call IS unconditional in the decompile.)

3. **Theory C — The class registry's vtable[10] for protagonist is something OTHER than `sub_5B7010` at runtime.** We resolved 0x6CC9D0 from the IDB; if the engine patches the vtable at startup (it does: dxwrapper-style hooks), the runtime address could be different. Worth a runtime check.

## Strategic implication

The "cheap factory call" approach is much harder than predicted. The factory's contract assumes `entity+0x1EC` is set up by some pre-step that we're not running. That pre-step is invisible from static analysis of `entity_factory_construct` — it must come from a higher-level spawn pipeline.

**Two next-step options**, in priority order:

1. **Phase 0C-step-2b: Find the primary spawn path** (~2-4h)
   Hypothesis: the engine has a separate `level_load_player_pipeline` that allocates the protagonist with a properly-seeded bag chain. Search:
   - Look for callers of `sub_5832C0` with the immediate `0xCCC` (= 3276 bytes, protagonist size). These are protagonist-allocation sites; one of them likely is the engine's primary path.
   - Look for callers of `sub_5B6F40` (the protagonist ctor body). If only `sub_5B71C0` calls it, the alloc-and-init is centralized; but if other sites call it post-alloc with extra setup, those are the paths we want.
   - Look at level-load entry points — these eventually call `entity_factory_construct` indirectly via `.sx` script execution. The script-VM dispatch table (registered via `script_register_command`) is the bridge.

2. **Phase 0C-step-2c: Hook vtable[10] runtime address validation + global-scope `sub_5B1E10` log** (~1h)
   Cheap to verify Theory C: install a global (NOT probe-scoped) `sub_5B1E10` PRE-logger that just logs `entity+0x1EC` for every call. Run a normal level load (no probe fire). If `sub_5B1E10` fires with `entity+0x1EC != 0` for the player entity, then the engine DOES populate the chain somewhere we missed. If it never fires, then the engine's normal player spawn doesn't go through `sub_5B7010` and we need to find the alt path (back to step-2b).

## Files modified this session

- [src/mtr-asi/include/mtr/coop_spawn_probe.h](../../src/mtr-asi/include/mtr/coop_spawn_probe.h) — added `bbd10_pre` + `bbd10_arg` + `bbd10_null_bypass_fired` ProbeResult fields
- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp) — added 9th hook (`hk_bbd10`) with NULL-arg short-circuit, extended `bc=[...]` string

## Engine VAs newly characterized this step

| VA | Symbol | Notes |
|---|---|---|
| 0x5B71C0 | `protagonist_ctor_wrapper` | __thiscall(this=registry_entry, bag). **IGNORES its bag arg.** Body: alloc(3276) + sub_5B6F40. |
| 0x5B6F40 | `protagonist_ctor_body` | __thiscall(this=entity). Calls sub_5B3F10(7443432) (actor base ctor) + writes protagonist-specific fields at this+810..817. NO writes to +0x1EC. |
| 0x5B3F10 | `actor_base_ctor` | __thiscall(this, a2). Calls sub_5B37C0(this) at end. |
| 0x5B37C0 | `actor_field_zeroer` | __thiscall(this). **Explicitly writes 0 to entity+492 (= +0x1EC)**. So entity+0x1EC = NULL after ctor is GUARANTEED, not incidental. |
| 0x5BBD10 | `<unknown>` (audio/voice or scene-helpers iterator?) | __cdecl(a1) -> _DWORD*. Iterates 240-byte records starting at `a1+164`, scans for `!` (0x21) terminators. NOT null-safe at first deref `*(uint16_t**)(a1+164)`. |


## What NOT to do

- Don't believe the original Phase 0A audit's claims about factory signature, bag descriptor layout, or template syntax. All three were corrected this session.
- Don't pass NULL for `a4` — sub_5B20F0 dereferences it. Use `static const float kInitPos[3] = {0,0,0};` minimum.
- Don't use multi-KV templates with `bag_init_from_template`. Use `bag_set_kv_THUNK` for additional keys.
- Don't try to remove the validate1 force-pass without understanding what makes the 3 sub-validators pass (next session's work).
- Don't hook `sub_55AC90` / `sub_55AC40` directly — they're 6-byte SecuROM thunks. Hook the runtime-resolved real fns via the IAT slot pattern (same as `sub_55AF00`).

---

# Phase 0C-step-2c result (2026-05-10)

## What we did

Made `hk_actor_init` permanent (installed at `coop_spawn_probe::install()` instead of probe-scoped). Added a global-mode logger that logs every engine call to `sub_5B1E10` with:
- `this` (entity ptr)
- `entity+0x1EC` value (bag chain head)
- caller return address (`_ReturnAddress()`)

Capped at 1024 lines per session. Ran the autonomous `load-save-1-show-ingame` scenario which boots the game, loads save 1, and proves gameplay with the player active.

## What we observed

**281 STEP2C lines captured during a single boot+load. ALL 281 had `entity+0x1EC` non-NULL.**

| Caller (RA) | Count | What's there |
|---|---|---|
| `0x005B702E` | 263 | Inside `sub_5B7010`, just after the `call sub_5B1E10` instruction. This is the protagonist `vtable[10]` wrapper called from `entity_factory_construct` (and via `.sx`-VM dispatch). |
| `0x00543025` | 18 | Inside `sub_542DB0` (a level-load loop in 0x542DB0..0x5430F8 that walks a linked list and constructs entities via `sub_5B96A0` = `entity_factory_lookup_and_construct`, NOT `entity_factory_construct`). The call site is the alternate vtable[10] dispatch. |

Sample lines:

```
[22:39:24.738] STEP2C global sub_5B1E10: this=0F49DA60 +0x1EC=0x0F49D8F0 RA=005B702E (call #1)
[22:39:24.738] STEP2C global sub_5B1E10: this=0F49C2C0 +0x1EC=0x0F49C150 RA=005B702E (call #2)
[22:39:32.306] STEP2C global sub_5B1E10: this=0F8CBA30 +0x1EC=0x0F8CD360 RA=005B702E (call #3)
... (281 total)
```

Note the +0x1EC values are heap-resident and vary in offset relative to `this` — not a fixed stride. They are not embedded inside the entity, so they're separately-allocated bag descriptors.

## What this disproves and what it leaves

**Disproves:** "The engine's primary spawn doesn't go through entity_factory_construct" (claim from the 0C-step-2a checkpoint, derived from "0 direct xrefs" in IDA). When checked again with `find` in `code_ref` mode, **`entity_factory_construct` has 20+ direct callers** in the 0x417xxx..0x418xxx range (level-load / world-grid management) and at 0x43d1cc. The `.sx`-VM-only-dispatch claim was wrong.

**Confirms:** The engine's normal entity construction path:
- DOES go through `entity_factory_construct → sub_5B7010 → sub_5B1E10` (RA=0x5B702E proves this).
- DOES seed `entity+0x1EC` to non-NULL before `sub_5B1E10` runs.

**Leaves open:** WHERE exactly `entity+0x1EC` gets seeded. We followed the factory body line-by-line and could find no direct write to `+0x1EC` in any of:
- The ctor chain (`sub_5B71C0 → sub_5B6F40 → sub_5B3F10 → sub_5B37C0`) — `sub_5B37C0` zeros it.
- `bag_merge_into` (writes to `entity+0xC7C`, not `+0x1EC`).
- `sub_55AD20` (validate1).
- `sub_5B20F0` (transform_setup, writes pos+rot at `entity+0x58`/+0x70).
- `sub_5B2080` (vtable[13], reads `+0x1EC` but doesn't write).

**Static byte-pattern search for direct writes through the `[reg+1ECh]` displacement form** (`89 ?? EC 01 00 00`, `C7 ?? EC 01 00 00`, plus 8/16-bit variants) found 5 hits across the binary. ALL 5 are false positives:
- `0x50740e`, `0x54a9aa` — not inside a recognized function (probably SecuROM stubs / unreached code).
- `0x5318e0` (in `sub_5317E7`) — unrelated 4-float init at this+122..125 of a different class (writes value 0x3FE3D70A = ~1.78f).
- `0x5db19c` (in `sub_5DB180`) — sound-event callback (registered via `sub_581420` table in `sub_5DC750` — list includes "Footfall" string). Different class.
- `0x4c2ea3` (in `sub_4C2D00`) — mesh-buffer setup with `sub_5832C0(80)` allocation. Different class.

**Static `lea ?, [reg+0x1EC]` search** (potential indirect-write setups) found 7 sites, all in `sub_5B1750`, `sub_5B2080`, `sub_5B2840`, `sub_5B2250`, `sub_5B26B0`. Spot-check of `sub_5B2080` and `sub_5B2840`: both READ `+0x1EC..+0x1F8` (4-slot loop), not write.

## Strategic conclusion

The seed of `entity+0x1EC` happens **somewhere we cannot find statically.** Most likely candidates:

1. **A function called transitively from the factory body that writes `entity+0x1EC` via a base+small-offset addressing form** the byte-pattern search couldn't catch (e.g., `lea reg, [entity+0x1E0]; mov [reg+0xC], val` ⇒ effective offset 0x1EC but no `1ECh` immediate appears in the bytes).

2. **A `memcpy`/struct-copy that fills a contiguous range** including offset `+0x1EC` — would appear as `rep movsd` with no offset constant visible.

3. **A function we've audited that we mis-decompiled.** Either `bag_merge_into` (full decompile not yet read), `sub_55AD20` (full decompile not yet read), or the factory body itself contains a write in a path the decompiler reordered or dropped.

## Cheapest next step (Phase 0C-step-2d — runtime write detection)

Install a stack-walk-on-entry hook at `sub_5B1E10`. Capture the top 5–8 return addresses on the call stack at PRE. For an engine call (where we now KNOW `entity+0x1EC` is non-NULL), the stack walks back through the seed function. Cross-reference RAs against known function ranges; the seed function will appear distinctively in the engine call but NOT in our factory call.

Expected runtime: ~1h (~50 LOC, naked-stub PRE pattern + fixed-depth stack walk).

If that doesn't isolate the seeder cleanly, alternative:

**Phase 0C-step-2e (heavier)** — Hook `sub_5832C0` (the heap allocator, takes size) to log allocations of size 0xCCC (= 3276, protagonist size) AND every subsequent `mov [allocator_result+1ECh], val` style write within 100 instructions. Requires single-stepping or at least basic-block tracing, more invasive.

## What NOT to do

- Don't pursue the v2-coop transport plan yet. The factory contract is still incomplete.
- Don't try a NEW single-point bypass inside `sub_5B1E10`. The dependency is structural (two NULL derefs of `+0x1EC`, plus indirect via vtable[44] and property-key loops).
- Don't trust the "0 direct xrefs to entity_factory_construct" claim from the 0C-step-2a checkpoint — it was wrong (20+ direct callers found this step).

## Files modified this session

- [src/mtr-asi/include/mtr/coop_spawn_probe.h](../../src/mtr-asi/include/mtr/coop_spawn_probe.h) — added `set_actor_init_global_log` + `actor_init_global_log_count` public API.
- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp) — moved `hk_actor_init` install to permanent (`install()` time), added global-mode logging (rate-limited to 1024 lines), added `_ReturnAddress()` capture, removed `actor_init` from probe-scoped `BreadcrumbHookSet`.

## Engine VAs newly characterized this step

| VA | Symbol | Notes |
|---|---|---|
| 0x542DB0 | `level_load_entity_loop` | Walks linked list, calls `sub_5B96A0` (= `entity_factory_lookup_and_construct`, different from `entity_factory_construct`) for each entry, then runs same factory-body steps inline (validate1, validate2, vtable[10]). 18/281 STEP2C calls reach `sub_5B1E10` via this path. |
| 0x5B96A0 | `entity_factory_lookup_and_construct` | __cdecl(class_name, bag). Uses different registry (`dword_7429C8`, NOT `g_class_registry_head`) than the protagonist factory. 1-line wrapper around `vtable[+4](registry_entry, bag)`. |
| 0x5B7010 | `protagonist_vtable10` (= sub_5B7010) | `result = sub_5B1E10(this); if (result) { /* alloc 152 bytes if !this[3244] */ this[3240] = dword_7426C4; }`. The 0x5B702E RA captured in STEP2C is the post-call-to-sub_5B1E10 instruction here. |

## Phase 0C-step-2d result (2026-05-10) — engine path uses vtable[11], NOT factory's vtable[10]

**Status:** GREEN forward progress (decisive structural finding). Build = 667,136 bytes, scenario GREEN at 10.2s / 1873 frames.

### What was instrumented

`hk_actor_init` now captures up to 8 return-address-shaped dwords from the stack via `_AddressOfReturnAddress()` + range-filter for `[0x00401000, 0x00700000)`. The walk is heuristic (scanning, not strict frame-pointer chain) because MSVC compiles much of the engine with `/Oy` (omit-frame-pointer) — but the consistent prefix across many calls makes the actual chain obvious.

Both modes log the chain:
- **Probe (`g_observing=true`)** — bc[3.9] line shows the probe call's stack
- **Global (`g_global_actor_init_log=true`)** — STEP2D line shows every engine call's stack

### Headline finding

**The engine's normal path to `sub_5B1E10` runs via `vtable[11] → sub_424182 (SEH wrapper) → sub_5ADC56 → sub_5B7010 → sub_5B1E10`. Our probe's path runs via `entity_factory_construct → vtable[10] = sub_5B7010 → sub_5B1E10`. The two paths do NOT converge on `sub_5ADC56`. The factory's inline `vtable[10]` dispatch is a DIFFERENT entry point from the engine's normal vtable[11] dispatch.**

### Engine vs probe stacks (representative)

```
Probe (entity+0x1EC=NULL → CRASH):
  bc[3.9]  stack=[005B702E 0069E97B 005B97B1 005A04F0 005B71C0 — — —]

Engine call #3 (entity+0x1EC=non-NULL → OK):
  STEP2D  stack=[005B702E 0069E97B 005ADC70 0069E790 00543025 00583004 00583B03 —]

Engine call #279 (entity+0x1EC=non-NULL → OK):
  STEP2D  stack=[005D3751 005B702E 0069E97B 005ADC70 005B218C 0069E790 005B97B1 006C0749]

Engine call #280 (entity+0x1EC=non-NULL → OK):
  STEP2D  stack=[005B702E 0069E97B 005ADC70 005B218C 0069E790 005B97B1 005BB3D1 005BB469]
```

Slot [2] of every engine call is `0x005ADC70` (= inside `sub_5ADC56`, offset 0x1A — the next instruction after `call sub_5B7010`). Slot [2] of the probe is `0x005B97B1` (= inside `entity_factory_construct`, offset 0xC1 — the next instruction after the `vtable[10]` dispatch).

### Tally of immediate caller (RA[0]) across 281 STEP2D captures

| RA[0] | Count | Symbol | Note |
|---|---|---|---|
| `0x005B702E` | 198 | `sub_5B7010+0x1E` | Direct caller. Within `sub_5B7010`, after `call sub_5B1E10`. |
| `0x006A6CE4` | 64 | (unclassified, late-image) | Most likely a stolen-byte trampoline that ultimately routes back into `sub_5B7010` (no normal `sub_5B1E10` call site exists at this VA in code). |
| `0x00543025` | 18 | `sub_542DB0+0x275` | Inside the alt level-load loop noted in step-2c. |
| `0x005D3751` | 1 | `sub_5D3730+0x21` | Singleton — different actor-creation path. |

### `sub_5ADC56` decompile (the engine's main vtable[11] body)

```c
char __usercall sub_5ADC56@<al>(int seh_chain@<eax>, _DWORD *entity@<ecx>) {
  // Sets up SEH frame (v10[3] = &loc_69E790, v11 = -1)
  result = sub_5B7010(entity);          // <-- THIS is where 263+ stacks reach sub_5B1E10
  if (result) {
    // Property-driven post-init: reads from entity+0xC7C (= per-instance bag),
    // sets bits in entity[7]/[9]/[13], registers in scene categories, etc.
    // Does NOT write to entity+0x1EC.
  }
  return result;
}
```

Reachable only via `sub_424182` (a 2-line SEH-thunk that loads `eax = NtCurrentTeb()->NtTib.ExceptionList` then tail-calls `sub_5ADC56`). `sub_424182` itself is in a vtable at `0xF8B2AC` — vtable[11] of an actor class.

### Factory body (re-verified)

`entity_factory_construct` at 0x5B96F0 unconditionally dispatches `vtable[10]` near the end (`if (!(*(*entity + 40))(entity)) { delete; return 0; }`). The dispatch reaches `sub_5B7010 → sub_5B1E10`. Between ctor return and this dispatch, the body does:
1. Read 'name' property → write to `entity+0x54` (if NULL)
2. Bag-merge template into per-instance bag at `entity+0xC7C`
3. `validate1` (sub_55AD20)
4. `transform_setup` (sub_5B20F0) — writes pos+rot, dispatches `vtable[13] = sub_5B2080`
5. `vtable[10]` dispatch (the crashing one)

`sub_5B2080` (vtable[13]): only **READS** `entity+0x1EC..0x1F8` to call `sub_4C4960` on each non-NULL slot. It does NOT seed `+0x1EC`.

So **nothing in the factory body seeds `entity+0x1EC` between ctor return and vtable[10] dispatch**. The engine's normal entities have `+0x1EC` seeded BEFORE entering this code path — which means they reach `sub_5B1E10` via a DIFFERENT mechanism (vtable[11] via sub_5ADC56, with the entity already set up by some prior step).

### Strategic implication

The factory's vtable[10] dispatch path is essentially **unused for entities that need a primary bag chain at +0x1EC**. The engine creates such entities via (most likely):
- Save-load deserializer that constructs the entity, populates fields directly from saved data (including +0x1EC), then invokes `vtable[11]` to finalize.
- Level-init / scene-load pipeline (.sx scripts, KFM/MDB asset loaders) that build entities from asset descriptors, populate +0x1EC, then run `vtable[11]`.

**Coop spawn implication:** spawning P2 via `entity_factory_construct` with a freshly-built bag is fundamentally the wrong approach for protagonist-class entities — the factory's vtable[10] path crashes on `+0x1EC = NULL` because no engine code path seeds it. Two viable alternatives:

1. **Hook the save-load deserializer** to inject a P2 entity descriptor when loading. The deserializer must know how to populate `+0x1EC`. Find it by hooking on save-load entry and tracing entity construction.

2. **Find a `entity_clone(p1, new_input_device)` or `actor_duplicate(player_entity)` API** in the engine and call it. This bypasses the factory entirely; the clone path reuses the source entity's full state (including `+0x1EC`).

### Cheapest next-session step (Phase 0C-step-2e, ~1h)

Hook `sub_5ADC56` PRE to confirm `entity+0x1EC` is non-NULL on EVERY entry. Two outcomes:
- All non-NULL → seed is upstream of vtable[11] dispatch (= upstream of where the engine invokes `entity->vtable[11](entity)`). Next step: find what dispatches vtable[11] in the engine's level-load.
- Sometimes NULL → seed is INSIDE `sub_5ADC56` itself (between SEH setup and `sub_5B7010` call). Re-decompile carefully for hidden writes via SEH frame slots `v10[0..3]`.

Then either look for a vtable[11] dispatch caller (`call dword ptr [reg+2Ch]`) and trace upward, OR look for save-load entity reconstruction code.

### Files modified this step

- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp) — added `capture_stack_chain()` helper, extended `hk_actor_init` PRE log to include the 8-RA stack chain in both probe and global modes. Doc + tag bumped from STEP2C to STEP2D.

### Engine VAs newly characterized this step

| VA | Symbol | Notes |
|---|---|---|
| 0x424182 | `actor_vtable11_seh_thunk` | __thiscall(entity). Single-line `return sub_5ADC56(NtCurrentTeb()->NtTib.ExceptionList, this);`. Sole xref is its slot at vtable+0x2C of the actor class (vtable storage at 0xF8B2AC). |
| 0x5ADC56 | `actor_vtable11_post_init_body` | __usercall(seh_chain@eax, entity@ecx). SEH-protected wrapper around `sub_5B7010` + property-driven scene-category registration. Body does NOT write to `entity+0x1EC`. |
| 0xF8B2AC | (data) | vtable slot 11 of an actor class — points to `sub_424182`. The vtable's owning class is unidentified (find_bytes had only 1 ref to `sub_424182`); next step's stack-walk on sub_5ADC56 may identify it by `*entity` (vtable ptr). |
| 0x5B2080 | `actor_vtable13_post_transform` | __thiscall(entity). Reads 4 slots `entity+0x1EC..0x1F8`, calls `sub_4C4960` on each non-NULL. Does NOT seed `+0x1EC`. |

## Phase 0C-step-2e result (2026-05-10) — engine's player class is "wilbur" not "protagonist"; factory's vtable[10] dispatch is dead in engine flow

**Status:** GREEN forward progress (two decisive findings + Phase 0A audit correction). Build = 667,136 bytes. Scenario GREEN at 9.7s / 1872 frames.

### What was instrumented

`hk_registry_lookup` (sub_5A04F0 = `class_registry_lookup_by_name`) was already a probe-scoped breadcrumb. **Promoted to permanent install** at boot with the same dual-mode pattern as actor_init from step-2c:
- `g_observing=true` → probe-scoped breadcrumb (existing bc[0])
- `g_observing=false` → STEP2E global log line (class name + caller RA), rate-limited to 1024 lines/session

### Headline finding 1: engine's player class is "wilbur", NOT "protagonist"

699 factory class lookups captured during boot+load-save-1. Tally:

| Class | Count | Notes |
|---|---|---|
| `compactor` | 151 | Most common (level interactables) |
| `triggerbox` | 64 | |
| `compActor` | 58 | |
| `DisassemblerFX` / `BonelessKFM` | 55 / 55 | |
| `BasicMover` | 40 | |
| `Interactable` | 23 | |
| `actor` | 18 | |
| `GenericNetActor` | 16 | |
| ...20+ more component classes... | | |
| **`wilbur`** | **1** | RA=0x005B9710 (= `entity_factory_construct`+0x20). Followed by 40+ sub-component class lookups from RA=0x005CEF91 — the wilbur ctor's component compositor. |
| **`protagonist`** | **0** | **NEVER looked up.** |

This **corrects the Phase 0A audit**: protagonist is in the registry but is NOT the class the engine spawns for the player. Player class is `wilbur` — a heavy compositor with ~40 sub-component instances.

### Headline finding 2: factory's vtable[10] dispatch is dead in engine flow

0 of 281 STEP2D `sub_5B1E10` captures had RA[0] inside the factory body (`0x005B97xx`). The factory dispatches vtable[10] at offset 0xC3 (next-instruction RA = 0x005B97B5), but no captured call has that RA. The factory's inline vtable[10] dispatch path **never reaches `sub_5B1E10` during normal engine operation**.

`sub_5B7010` always calls `sub_5B1E10` first thing, so for every entity created during boot+load-save-1, the factory's vtable[10] dispatch either (a) doesn't fire, or (b) dispatches to a non-`sub_5B7010` chain.

Most likely: **`validate1` returns 0 in the engine's natural path**, factory deletes entity and returns NULL early, no vtable[10] dispatch happens. Our probe force-passes validate1 — that's the ONLY reason the probe reaches vtable[10] and crashes. The engine never relies on factory's vtable[10] for these entities; entities are set up via the separate vtable[11]-only mechanism (with manual `+0x1EC` seed) per step-2d.

### Headline finding 3: `class=wilbur` did NOT fix the seed

Re-ran the probe with `kTemplate = "class=wilbur"`. The wilbur ctor performed its full sub-component fan-out (40+ class lookups), then bag_merge, validate1 (returned 0, force-passed), transform_setup, and finally `sub_5B1E10` PRE — `entity+0x1EC = 0x00000000` STILL. The wilbur ctor itself doesn't seed `+0x1EC` either.

Bc[3.9] stack with class=wilbur:
```
stack=[005B702E 0069E97B 005ADC70 0069E790 005B97B1 005B71C0 004B95A0 —]
```
Now contains `0x005ADC70` (= sub_5ADC56) at slot[2]. May be stale stack OR may indicate the wilbur ctor recursively enters the SEH-wrapped path internally. Either way, `+0x1EC` stays NULL.

### Strategic conclusion

The engine's wilbur creation flow appears to be:
1. Level-load fn calls `entity_factory_construct(class=wilbur, ...)`.
2. Factory: ctor + bag_merge + validate1.
3. **`validate1` returns 0** (factory's body intentionally rejects "headless" wilbur construction).
4. Factory deletes entity, returns NULL.
5. Caller handles the NULL via a SEPARATE wilbur creation path that DOES seed `+0x1EC` (likely a save-load deserializer or `wilbur::CreateFromSave` style API).
6. Caller dispatches `vtable[11] → sub_424182 → sub_5ADC56 → sub_5B7010 → sub_5B1E10` with `+0x1EC` seeded.

For coop spawn: the `entity_factory_construct(class=wilbur)` approach is wrong even with the right class. The right path is whatever step (5)-(6) above is.

### Cheapest next-session step (Phase 0C-step-2f, ~1h)

Hook `entity_factory_construct` itself at PRE+POST. Capture: class name (from bag), caller RA, bag descriptor, return value. For the engine's wilbur factory call:
- If POST returns NULL: factory failed at validate1 — wilbur entity is NOT created via the factory. Hook the wilbur registry's vtable[+4] (the wilbur ctor) directly to find what calls it OUTSIDE the factory.
- If POST returns non-NULL: factory succeeded, seed happens after factory returns. Identify which of the 20+ direct factory callers is the wilbur caller, then trace what it does post-factory.

### Files modified this step

- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp):
  - Promoted `hk_registry_lookup` to permanent install with global STEP2E logger.
  - Added `g_global_registry_log` + `g_global_registry_log_count` atomics.
  - Removed `registry_lookup` from `BreadcrumbHookSet` (permanent now); removed install/uninstall from probe-scoped paths.
  - Changed probe `kTemplate` `class=protagonist` → `class=wilbur` (engine's actual player class).

### Engine VAs newly characterized this step

| VA | Symbol | Notes |
|---|---|---|
| 0x5B9710 | `entity_factory_construct+0x20` | RA after the factory's `class_registry_lookup_by_name` call. Marker for "this lookup came from inside factory body". |
| 0x5CEF91 | (unidentified, near 0x5CExxx) | RA inside wilbur ctor's component compositor. Fires 40+ times per wilbur factory call to look up sub-component classes (SimpleGroundController, WilburDriver, ActionHandler, GroundFollower, …). Indicates wilbur is implemented as a heavy compositor of ~40 sub-component instances. |

## Phase 0C-step-2f result (2026-05-11) — engine's wilbur factory call SUCCEEDS; our probe's bag is inadequate

**Status:** GREEN forward progress (decisive reframing). Build = 668,672 bytes. Scenario GREEN at 9.5s (first try!) / 1870 frames.

### What was instrumented

Added permanent hook on `entity_factory_construct` (sub_5B96F0). Logs every factory call:
- PRE: bag descriptor, pos, rot, caller RA, call#
- POST: return value (entity ptr or NULL), call#

### Headline: only 4 factory calls during boot+load-save-1

| # | PRE timestamp | bag | pos | rot | RA | POST | Result |
|---|---|---|---|---|---|---|---|
| 1 | 00:14:23.505 | 0x030FFD74 | 0x00728A84 | -2.8126 | **0x020FD13B** | 0x0EF32FE0 | **SUCCESS** (= the wilbur call!) |
| 2 | 00:14:23.530 | 0x0F99A680 | 0x0F176510 | 0.0000 | 0x005BB3D1 | 0x0F126D80 | SUCCESS |
| 3 | 00:14:23.531 | 0x0F99AEE0 | 0x0F176468 | 0.0000 | 0x005BB3D1 | 0x0F054200 | SUCCESS |
| 4 | 00:14:24.180 | 0x030FEB60 | 0x549C0470 | 0.0000 | 0x54959674 | (none) | EXCEPTED (= our probe's call) |

Call #1's RA=0x020FD13B is OUTSIDE Wilbur.exe's image (high user-allocated memory, 0x02xxxxxx range). This is almost certainly a **`.sx` script VM JIT trampoline** — the wilbur entity is created by an `.sx` script during scene-load. The script builds a bag with all necessary KV pairs, then calls the factory.

Calls #2 and #3 had RA=0x005BB3D1 (= inside `sub_5BB2C0`, the SecuROM-thunk wrapper noted in step-2d).

### Key reframing

The factory IS the right entry point for wilbur creation. The engine's call SUCCEEDED. Our probe's call FAILED only because **our bag is inadequate** — just `class=wilbur; name=player2`. The engine's bag (built by the `.sx` script) contains all the KV pairs the factory needs to construct a working wilbur instance.

This **partially walks back** step-2d/2e's strategic conclusion ("factory is wrong path"). The factory IS the path; we just need the right bag.

### Note on STEP2E's 699 lookups

Of 699 `class_registry_lookup_by_name` calls:
- **407 from RA=0x005CEF91** — wilbur ctor's component compositor (looking up component CLASS DESCRIPTORS, not constructing them as full entities; sub-components are likely instantiated lazily or stored as type metadata).
- **289 from RA=0x005B96B0** — inside `entity_factory_lookup_and_construct` (sub_5B96A0, the OTHER factory function distinct from `entity_factory_construct`). Used by the level-load loop sub_542DB0.
- **3 from RA=0x005B9710** — inside `entity_factory_construct` itself.

Combined: most class lookups are NOT done via `entity_factory_construct`. Both factories share the registry but use different registry heads (per F1 from step-2c).

### Why the wilbur entity (0xEF32FE0) doesn't appear in 281 STEP2D captures

The wilbur factory call's returned entity (0xEF32FE0) is NOT among the 281 `this` values in STEP2D. None of those captures correspond to the wilbur entity itself. Possible explanations:

1. **Wilbur's vtable[10] is NOT sub_5B7010** — wilbur is a sufficiently-different subclass that overrides vtable[10] to a function that doesn't call sub_5B1E10. The factory's inline vtable[10] dispatch on wilbur calls a different post-init function.
2. **Wilbur entity is a passive composition-of-actors** — the ACTUAL gameplay entities are wilbur's 40+ sub-components, each of which is an actor that fires sub_5B1E10 via vtable[11]. The 3 STEP2D captures (#279-281, immediately after wilbur factory returned) are likely 3 of those sub-components.
3. **vtable[11] dispatch on wilbur happens later** — possibly never within our 1870-frame window.

For the coop pivot, we don't immediately need to know why — we just need a bag that produces a SUCCESS factory return.

### Cheapest next-session step (Phase 0C-step-2g, ~1.5h)

**Capture the engine's wilbur bag contents.** Hook `bag_merge_into` (sub_4B95A0) PRE permanently. When called from RA=0x005B9778 (= inside `entity_factory_construct`+0x88, the merge-step site), traverse the source bag and log every KV pair. The wilbur factory call's bag-merge will dump its full content. We then replicate those KV pairs in our probe's bag and re-test.

Alternative: directly hook `bag_init_from_template` and `bag_set_kv` PRE permanently to log everything written to bags during the engine's wilbur factory call. We already have these functions typed in the probe.

### Files modified this step

- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp):
  - Added `g_global_factory_log` + `g_global_factory_log_count` atomics.
  - Added `hk_factory_construct` __cdecl detour (PRE+POST logging).
  - Installed permanently in `install()`.

### Engine VAs newly characterized this step

| VA | Symbol | Notes |
|---|---|---|
| 0x020FD13B | (.sx script VM JIT) | Caller of the engine's only wilbur factory call. Outside Wilbur.exe image (high user-allocated memory). The wilbur entity is created BY A SCRIPT during scene-load. |
| 0x5B9778 | `entity_factory_construct+0x88` | RA after the factory's `bag_merge_into` call — useful for distinguishing factory-internal merges from external ones. |
| 0x5B96B0 | `entity_factory_lookup_and_construct+0x10` | RA after the other factory's class_registry_lookup. 289/699 STEP2E lookups came from here (level-load loop in sub_542DB0). |

---

## Phase 0C-step-2g + step-2h result (2026-05-11) — engine wilbur bag captured + replicated; probe reached sub_55AF00 (post-init) before crashing

### Step-2g: bag_merge_into permanent hook + factory-RA-filtered KV walk

Promoted `hk_bag_merge` to a permanent install with dual-mode (probe sentinel + factory-RA-filtered global dump). When `bag_merge_into` (sub_4B95A0) is invoked from inside `entity_factory_construct` (RA == 0x005B977D, the 5-byte CALL at 0x5B9778), the hook walks the SRC bag's linked KV list and logs every `(key, value)` string pair as STEP2G dump lines. bag_merge_into has exactly 4 callsites and only this one is inside the factory body, so the filter is both precise (no false positives) and sufficient (catches every factory call's bag).

Bag layout (per `bag_merge_into` decompile at 0x4B95A0):
- src is `void**`; `*src` is the head node; subsequent nodes via `node[0]`.
- each node: `[0]` next, `[1]` key char*, `[2]` value char* — RAW strings before intern hashing.

### Engine's wilbur bag — exact 2-KV set

STEP2G dump #1 (engine's wilbur factory call, bag=0x030FFD74 from the .sx script VM caller at RA=0x020FD13B):

```
KV[00]: key="model_name" val="avatars/wilbur_low"
KV[01]: key="class"      val="wilbur"
total 2 KVs
```

**The engine's wilbur bag is 2 KVs only** — much smaller than the 45-KV monster bags we see for compActor NPCs (Lazlo dump #2 = 45 KVs, Carl dump #3 = 41 KVs). NO `name=` key.

Pre-step-2g, our probe used `class=wilbur` + `name=player2`. The right second KV is `model_name=avatars/wilbur_low`, not `name=`.

### Step-2h: probe replicates engine bag → 8-breadcrumb-deep success

Updated probe to use `class=wilbur` template + chained `model_name=avatars/wilbur_low`. Re-ran `load-save-1-show-ingame -Redeploy`. Process crashed at the OS level (exit code 0xC0000005), but the probe's SEH caught its own exception cleanly. The breadcrumb trail captured got the FURTHEST EVER — 8 successful steps before crashing:

| bc | Hook | Result |
|---|---|---|
| bc[0] | registry_lookup "wilbur" RA=0x5B9710 | OK (factory-internal class dispatch) |
| bc[3] | bag_merge_into (our 2-KV bag) | OK |
| **bc[3.5/3.6]** | **validate1 sub_55AD20** | **PASSED NATIVELY (result=1, no force-pass)** |
| bc[3.7/3.8] | transform_setup | OK |
| **bc[3.9/3.95]** | **actor_init sub_5B1E10** | **entity+0x1EC=0x0F00E5E0 NON-NULL; POST result=1** |
| bc[3.91] | sub_5BBD10 arg=0x0F00E5E0 (non-NULL) | OK (no bypass needed) |
| bc[4a] | scene register active sub_5AD410 | entered |
| bc[5] | sub_55AF00 (post-init) PRE: **this=NULL** v13=entity | crashed during the original call |

Stack chain at actor_init PRE: `[005B702E 0069E97B 005ADC70 0069E790 005B97B1 0055AD20 005B71C0 00000000]` — matches the engine's own vtable[11] path (slot[2]=005ADC70) at the bottom and the factory's vtable[10] path (slot[4]=005B97B1) at the top. So our entity transitions naturally between the factory body and the engine's normal actor-init flow.

Notable: `bc[1]/bc[2]` (ctor) did NOT fire — meaning the **actual wilbur ctor is NOT sub_5B71C0** (we had hooked that based on Phase 0A audit, but the factory's class-dispatch for wilbur goes to a different vtable[+4]). The entity was still created and seeded correctly via whatever ctor wilbur's registry entry points to — we just don't have a hook on it.

### Strategic reframe (step-2h)

The factory IS the correct entry point for wilbur creation. The bag is now correct. Everything that used to crash now passes. The new (and far more localized) failure point is **sub_55AF00 (post-init) being called with this=NULL**. Two possible causes:
1. `this` is some scene/level/global-state pointer we haven't initialized (= sub_55AF00 expects to be called only after some prior global state setup that the .sx script driving the engine's natural wilbur creation provides).
2. There's an upstream consumer of the factory-returned entity that hasn't been set up, and the NULL is incidental damage rather than the actual crash cause.

### Cheapest next-session step (Phase 0C-step-2i, ~1h)

1. RE the call site of sub_55AF00 inside `entity_factory_construct` (or wherever the factory tail calls it). What is the `this` pointer there? Read what the engine passes in the natural flow (step-2f showed call #1 = engine's wilbur call SUCCEEDED, so post-init must have been called with a non-NULL this).
2. Two options once `this` is identified:
   - **Bypass**: NULL-check + short-circuit sub_55AF00 the same way we did sub_5BBD10. Likely safe because if this=NULL, the fn body can't do anything useful.
   - **Provide**: figure out what state initializes the `this` value and ensure the probe has set it up before calling factory.

### Files modified this session (step-2g + 2h)

- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp):
  - Added `g_global_bag_dump_count` atomic + `copy_str_safe()` + `dump_factory_bag()` helpers.
  - Updated `hk_bag_merge` to dual-mode: probe sentinel + factory-RA-filtered (`kFactoryMergeRA=0x005B977D`) STEP2G KV dump.
  - Promoted `bag_merge_into` hook to permanent install; removed from `BreadcrumbHookSet` + probe-scoped install/uninstall.
  - Step-2h: switched probe's chained KV from `name=player2` to `model_name=avatars/wilbur_low` to match engine's exact wilbur bag.
- [research/findings/coop-phase-0b-breadcrumb-trail-2026-05-10.md](.) — this section appended.

### Engine VAs newly characterized

| VA | Symbol | Notes |
|---|---|---|
| 0x4B95A0 | `bag_merge_into` (re-confirmed) | Now hooked permanently with factory-RA-filtered KV walker. Bag node layout: `[0]=next, [1]=key char*, [2]=value char*`. |
| 0x5B977D | `entity_factory_construct+0x8D` | RA pushed onto stack by the factory's `call bag_merge_into` at 0x5B9778. Exact-equality filter for step-2g dump. |
| 0x55AF00 | `sub_55AF00` (post-init, IAT-resolved) | New crash site. Called with this=NULL in our probe path; engine path presumably passes a non-NULL state pointer. |


## Phase 0C-step-2i — calling-convention correction for sub_55AF00 (2026-05-11)

### Headline

**The `this=NULL` snapshot from step-2h was a phantom.** Re-RE'd both call sites of sub_55AF00 by disasm and confirmed: `sub_55AF00` is **`__cdecl(entity, prop_value)`** at the engine level, NOT `__thiscall`. The probe's previous `__fastcall` hook signature was reading a stale/random ECX as "this" — the actual data the engine pushes is (entity, prop_value) on the stack with caller cleanup.

### Evidence (engine call sites)

Both xrefs to sub_55AF00 show the same pattern:

`entity_factory_construct` @ 0x5B9807:
```
push    eax            ; prop_value (returned by entity_property_get_thunk("key=0x6A8888"))
push    esi            ; entity ptr (= v9 = factory-created entity)
call    sub_55AF00
add     esp, 8         ; CALLER cleanup of 2 stack args => __cdecl
```

`sub_542DB0` @ 0x54309E (level-load entity instantiator):
```
push    eax            ; prop_value
push    esi            ; entity ptr
call    sub_55AF00
add     esp, 8
```

Neither caller sets ECX before the call. The IDA decompile's `__thiscall(this, a2, a3)` was a phantom inferred from the 6-byte JMP-thunk wrapper, not from actual caller behavior.

### Why this matters for the crash

The probe's prior hook signature was:

```cpp
int __fastcall hk_post_init(void* this_, void* /*edx*/, int v13);
```

When the engine reached sub_55AF00 with its real `__cdecl(entity, prop)` push pattern, the hook:
1. Mis-labeled stale ECX as "this" → logged "this=NULL" even though the engine never set ECX.
2. Forwarded `g_orig_post_init(this_, nullptr, v13)` — a C-level `__fastcall` call with only ONE stack arg (`v13`). The trampoline JMPs to the real fn with **`[esp+8]` = garbage** (whatever happened to be there), so the real fn body read junk for `prop_value` and faulted on it.

So the `this=NULL` snapshot was NOT a structural issue with engine state. It was the hook itself corrupting the call into the real fn.

### Fix shipped

[src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp):

```cpp
// Re-typed to match the engine's real convention:
using PFN_PostInit = int (__cdecl*)(void* entity, int prop_value);

int __cdecl hk_post_init(void* entity, int prop_value) {
    if (g_observing.load(std::memory_order_acquire)) {
        g_observed_this.store(entity, std::memory_order_release);
        g_observed_v13.store(reinterpret_cast<void*>(
                                 static_cast<uintptr_t>(prop_value)),
                             std::memory_order_release);
        g_observed_fired.store(true, std::memory_order_release);
        mtr::log::info("coop_spawn_probe: bc[5] sub_55AF00 PRE (__cdecl):"
                       " entity=%p prop_value=0x%X",
                       entity, static_cast<unsigned>(prop_value));
    }
    return g_orig_post_init(entity, prop_value);
}
```

The `g_observed_v13` slot now correctly stores `prop_value` (which is what IDA pseudo-code variable `v13` actually is — the slot name was right, only the hook's interpretation was off). `ProbeResult::post_init_v13_arg` field semantics unchanged.

### Expected outcome (pending manual test run)

If the hook bug WAS the crash trigger:
- factory call should now return non-NULL.
- bc[5] PRE log should show `entity=<our-spawned-entity>` and a non-zero `prop_value`.
- Probe returns success.

If the crash persists:
- The fault is genuinely inside sub_55AF00's body (real fn dereferences some entity field that our spawn-path doesn't initialize).
- Next step would be to disasm the real fn (resolved via IAT slot 0x00F8DED0 at runtime) and find the first dereference inside.

### Files modified this session (step-2i)

- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp):
  - `PFN_PostInit` retyped from `__fastcall(this_, edx, v13)` to `__cdecl(entity, prop_value)` with full convention-correction docblock.
  - `hk_post_init` rewritten as `__cdecl(entity, prop_value)`; trampoline call updated to forward both stack args.
  - Log format string updated to reflect the actual semantics.

### Engine VAs (step-2i additions)

| VA | Symbol | Notes |
|---|---|---|
| 0x5B9807 | `entity_factory_construct` post-init call site | `push eax; push esi; call sub_55AF00; add esp, 8` — caller cleanup of 8 bytes confirms __cdecl. |
| 0x54309E | `sub_542DB0` post-init call site | Same exact pattern as 0x5B9807. Independent corroboration of __cdecl convention. |



### Test run outcome (2026-05-11 11:18 — assistant-launched with explicit user authorization)

`pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy`:

- `try_spawn_p2 returned true`. Factory constructed entity **0EE81A20** (wilbur instance).
- Full breadcrumb trail fired (9/9): `bc=[reg=1, merge=1, v1_pre=1/post=1/res=1, tx_pre=1/post=1, ai_pre=1/post=1/res=1, bbd10_pre=1/byp=0, a=1, q=0, pi=1]`.
- `bc[5] sub_55AF00 PRE (__cdecl): entity=0EE81A20 prop_value=0x0` — convention fix worked. `prop_value=0` is expected (our 2-KV bag has no key 0x6A8888h).
- `STEP2F factory POST: return=0EE81A20 (call #4 SUCCESS)`.

**Process then crashed 0xC0000005 ~150ms later.** Last log line = post-probe screenshot at 11:18:17.700. The fault is **downstream of the factory** — engine sim/render integration of our orphan wilbur on the next tick. The factory primitive itself is correct.

Step-2j defines the catch-the-crash plan (vectored exception handler) — see project_state_2026-05-11_coop_phase_0c_step2i_cdecl_fix.md.


## Phase 0C-step-2j — VEH crash capture (2026-05-11)

### Setup

Added a vectored exception handler in `coop_spawn_probe::install()` with `FirstHandler=1`, one-shot via `g_veh_fired`. Logs first fatal CPU exception (AV, illegal-insn, priv-insn, div0, stack overflow, array-bounds) with EIP, all 8 GPRs, AV operand+addr, and ESP[0..15] dump. Returns `EXCEPTION_CONTINUE_SEARCH` so the OS still kills the process — we just get diagnostics before it does.

### Captured crash (test 2026-05-11 11:23)

```
STEP2J VEH FAULT code=0xC0000005 eip=0x005CB163 av_op=0 av_addr=0x00000004
STEP2J regs eax=0 ebx=0x76E94500 ecx=0 edx=0 esi=0 edi=0x0EFB0A20 ebp=0x0F98C360 esp=0x030FFD74
STEP2J stack[00..03]=0EFAC400 005454EA 0EFB0A20 0EFACD0C
STEP2J stack[04..07]=00000003 005EBA8E 3D889A02 0EFB0A20
STEP2J stack[08..11]=0EFACD24 0EFAE440 005AD9DC 0F1D02D0
STEP2J stack[12..15]=005C85D5 0F5C9550 0F5C9530 005AD646
```

Faulting instruction (`sub_5CB160 + 3`):
```
5cb160  push    esi
5cb161  mov     esi, ecx       ; this -> esi (ecx = NULL → esi = NULL)
5cb163  mov     al, [esi+4]    ; ← FAULT: read 1 byte at 0x00000004
```

ECX (the `__thiscall this`) was NULL on entry — sub_5CB160's caller passed NULL.

### `sub_5CB160` (the faulting function)

```c
int __thiscall sub_5CB160(_DWORD *this)
{
  if ( *((_BYTE *)this + 4) )         // ← fault at this+4 with this=NULL
  {
    v2 = (int (__cdecl *)(_DWORD, _DWORD *))*(this + 2);
    if ( v2 ) {
      if ( v2(*this, this) ) *((_BYTE *)this + 4) = 0;
    }
  }
  if ( *(this + 4) == 5 ) return *(_DWORD *)*(this + 3);
  else return 0;
}
```

A 20-byte deferred-callback / state-machine object:
- `+0x00`  object ptr (subject)
- `+0x04`  pending flag
- `+0x08`  callback function pointer
- `+0x0C`  result-pointer (result-of-result)
- `+0x10`  state code (5 = done)

50+ xrefs across the engine — it's a generic "future/promise resolver" called from many subsystems.

### Caller chain (deepest → shallowest)

| # | Frame | Returns into | Notes |
|---|---|---|---|
| 1 | crashed | inside sub_5CB160 head | `mov al, [esi+4]` with esi=NULL |
| 2 | unanalyzed fn ~0x5454xx | 0x5454EA | IDA has not auto-defined this region (0x545454..0x5458BF undefined). Vtable[17] target dispatched from sub_55D8F0. |
| 3 | sub_55D8F0 | 0x5EBA8E (inside sub_5EBA80) | At 0x5EBA89: `call sub_55D8F0`. sub_55D8F0 does `*((this)[17])(this, hint_ptr)` — calls a vtable[17] target on its arg. |
| 4 | sub_5EBA80 (vtable[6] target's caller-of-caller?) | 0x5AD9DC (inside sub_5AD9B0) | sub_5AD9B0 walks `(this+833)` list, calling `vtable[13]` on each entry at 0x5AD9D9. |
| 5 | sub_5AD9B0 → some intermediate | 0x5C85D5 (~0x5C84xx fn unnamed) | Returns deeper into intermediate at 0x5C85D5. |
| 6 | intermediate → sub_5AD4D0 | 0x5AD646 (inside sub_5AD4D0) | sub_5AD4D0 calls `(scene+0x20).vtable[1]()` at 0x5AD643. **This is the scene/entity-manager tick entry.** |

### Diagnosis

`sub_5AD4D0` (the big scene-tick at `g_entity_manager_ptr`) calls a subsystem at scene+0x20 → walks a list at this+833 → each entry's vtable[13] dispatch → eventually reaches a future/promise resolver (sub_5CB160) for some sub-component WITH a NULL pointer where a valid promise object should be.

**Crucial diagnostic clue: `edi = 0x0EFB0A20`** (presumed current entity being processed) — this is **NOT our spawned orphan** (which was `0x0EE81A20`). The chain processes many entities (stack shows `0EFAC400`, `0EFB0A20`, `0EFACD24`, `0EFAE440`) — typical scene tick walking every active entity.

So the crash is on a **DIFFERENT entity than the one we spawned**. Three possibilities:
1. Our orphan's `register_active` mutated a global subsystem registry inconsistently; another entity's scene-tick now reads garbage from the disturbed registry.
2. Our orphan was inserted into a list expected to be NULL-terminated and now an unrelated walker reads past expected end.
3. Our orphan's component compositor wrote to a shared structure (e.g. weapon-inventory registry) that another entity now reads as inconsistent.

### Cheapest next-session step (Phase 0C-step-2k, two options)

**Option A — Stability via tear-down** (~1h, lowest risk):
Tear down our spawned entity immediately after the probe records success, before returning from `try_spawn_p2`. Factory's own fail-path uses `(**(void (__thiscall ***)(int, int))v9)(v9, 1)` (vtable[0] with arg=1) — that's the entity destructor. Call it on success too, after capturing the proof-of-life metadata. Confirms spawn primitive works AND keeps engine stable for further iteration.

**Option B — Identify and skip the disturbed list** (~3h, gives deeper coop infra):
1. Hook `sub_5AD9B0` PRE+POST or instrument it as a permanent logger to capture (entity, sub-component, vtable[13] target) for every entry it processes during the post-probe sim tick.
2. The fault's `edi=0x0EFB0A20` traces back to one of the entries — find which one, decompile its vtable[13] target, identify the missing sub-component.
3. Either:
   - Seed the missing sub-component on our orphan before registering, OR
   - Skip the orphan in the disturbed list's walker hook.

**Recommendation: Option A first.** It proves the spawn primitive permanently works (currently the test exits 0xC0000005 even though `try_spawn_p2` returned true — Option A would make the test exit 0). Option B can come later when we're actually wiring coop input routing.

### Files modified (step-2j)

- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp):
  - Added `g_veh_fired` atomic + `g_veh_handle` PVOID.
  - Added `veh_crash_logger()` — fatal-CPU-exception filter, one-shot, logs EIP/regs/ESP[0..15].
  - Installed VEH in `install()` via `AddVectoredExceptionHandler(1, ...)`.

### Engine VAs (step-2j additions)

| VA | Symbol | Notes |
|---|---|---|
| 0x5CB160 | `sub_5CB160` (future/promise resolver) | 50+ callers across engine. 20-byte struct: +0=subject, +4=pending byte, +8=callback fn ptr, +12=result-ptr-ptr, +16=state code. Crash when called with this=NULL. |
| 0x5AD9B0 | `sub_5AD9B0` (subsystem tick — list walker) | Walks `(this+833)` doubly-linked list; for each entry does `(*(*entry)->vtable[52/4=13])()`. One node in the path that reaches sub_5CB160 with NULL. |
| 0x5AD4D0 | `sub_5AD4D0` (scene tick) | Big scene/entity-manager tick. Calls many subsystems via vtable. Calls `(this+0x20).vtable[1]()` at 0x5AD643. Returns to 0x5AD646 in the call chain. |
| 0x545454..0x5458BF | (unanalyzed code region) | IDA has not auto-defined. The caller of sub_5CB160 lives here (return addr 0x5454EA). Likely a vtable[17] target reachable from sub_55D8F0. Future RE step. |

## Phase 0C-step-2k — orphan teardown shipped (2026-05-11)

### Fix

Added a `vtable[0](entity, 1)` call inside `try_spawn_p2` after `set_last(r)` records success, before returning. This is the same call pattern `entity_factory_construct` uses in its own fail-paths (after `validate1` or `actor_init` returns false). It invokes the MSVC scalar deleting destructor (`free_flag=1`), whose body handles unregistration from scene/manager lists and frees the entity memory. Wrapped in SEH because an orphan dtor may itself fault on uninitialized fields.

### Test result (2026-05-11 11:32, assistant-launched with explicit user authorization)

```
scenario=load-save-1-show-ingame result=pass elapsed_ms=9109 frames=1870
exit code: 0
```

Probe log:
```
factory returned 0EED4A20 delta=0 (queued/orphan) bc=[reg=1,...,a=1,...,pi=1] v13=...
STEP2K orphan teardown: vtable[0](0EED4A20, 1) returned cleanly
load-save-1: try_spawn_p2 returned true; ...
TESTHARNESS: scenario=load-save-1-show-ingame result=pass elapsed_ms=9109 frames=1870
```

No STEP2J VEH fired — engine remained stable through to the harness's normal "show in-game" exit.

### Closing the loop on Phase 0C

This completes the Phase 0C derisk experiment. We've proven:

1. **The entity factory CAN be invoked from the mod for class=wilbur** (step-2g..2i).
2. **The factory returns a non-NULL entity, fully constructed**, with all 9 breadcrumbs firing (registry lookup, bag merge, validate1 PASS, transform setup, actor init, sub_5BBD10, scene register, post_init).
3. **The engine stays stable around the spawned entity** as long as the orphan is destroyed before the next sim tick (step-2k).

The cheap-information experiment defined back in [coop-phase-0a-audit-2026-05-10.md](coop-phase-0a-audit-2026-05-10.md) Option A has succeeded. The factory IS the gate for coop player2 spawning, the bag KV set required is small (2 keys: `model_name=avatars/wilbur_low`, `class=wilbur`), and we now have a working primitive.

### What's NOT yet solved

A **persistent** orphan entity (= one that survives past the spawn frame) needs more work. The crash in step-2j showed the orphan disturbs scene state somewhere. To keep an orphan alive for coop input routing in Phase 2, we'll need to either:
- Identify what global registry/list the orphan corrupts (the `(this+833)` list walker in sub_5AD9B0 with vtable[13] dispatch path → unanalyzed fn at 0x5454xx → sub_55D8F0 vtable[17] → sub_5CB160 future/promise resolver) and either seed it properly or skip it.
- Or use a different spawn entry point that doesn't auto-register into the disturbed structure.

That's Phase 2 work, not Phase 0C. The derisk experiment is DONE.

### Files modified (step-2k)

- [src/mtr-asi/src/coop/coop_spawn_probe.cpp](../../src/mtr-asi/src/coop/coop_spawn_probe.cpp):
  - Added `vtable[0](entity, 1)` teardown call inside `try_spawn_p2`, SEH-wrapped, after message log and before set_last.

### Result for the v2 plan

The coop v2 plan ([project_coop_multiplayer_plan_v2_ready_2026-05-10.md](project_coop_multiplayer_plan_v2_ready_2026-05-10.md)) Phase 0 was capped at ~1wk after Phase 0A came in 1hr. Phase 0C (the derisk) took the rest of that time, ~1day spread across two sessions. The 9-10mo total estimate stands — Phase 0 finished within budget. Phase 1 (UDP transport) is the next major milestone.

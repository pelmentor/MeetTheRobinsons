# Coop Phase 0 — Real gameplay-input separation point identified (2026-05-11 evening)

> Follow-up to the F2 retraction in [coop-phase0-finding-f2-corrected-2026-05-11.md](coop-phase0-finding-f2-corrected-2026-05-11.md). With F2 retracted (the engine has **no** built-in input-separation primitive in the cheat-code path), the open question was: where IS the real per-player input separation point? This doc answers that.

## TL;DR

- **`g_input_mgr` at 0x745B70 IS a dead/orphan structure.** 4 xrefs total, all in `sub_526960`; the decompilation doesn't even visibly reference the address. Confirmed dead end for Phase 2.
- **The active gameplay input system is the `ControlMapper` singleton**, accessed via `sub_5CB310("ControlMapper", 0)` then `sub_5CB160(slot)` to deref. **60 immediate-32 callers** of that pattern in the binary.
- **The ControlMapper API has NO `player_idx` parameter.** Methods are `vtable[N](this, button_id_or_other)`. There is exactly ONE ControlMapper instance, registered under the name "ControlMapper".
- **Therefore Phase 2 separation point** = a ControlMapper proxy layer. Two viable intercepts:
  1. **Registry-level**: hook `sub_5D3730(name, len)` (the underlying name→slot lookup, called by both `sub_5CB310` and `sub_5CB350`) to return a proxy slot for "ControlMapper" lookups.
  2. **Vtable-level**: in-place patch ControlMapper's vtable to thunk each method through a player-context resolver.
- Phase 2 estimate (post-F2-correction) holds at **~3-4 weeks**, matching the v2 plan's restored estimate.

## Evidence

### 1. `g_input_mgr` (0x745B70) is orphan

Symbol-table memory + debug-features research correctly identified `g_input_mgr` as having a "per-player state array architecture" at `[+0x04]` (= 0x745B74). Live value reads:

| Offset | Bytes (LE) | Interpreted |
|---|---|---|
| 0x745B70 | `01 00 00 00` | player count = 1 |
| 0x745B74 | `00 AE 57 0F` | pointer 0x0F57AE00 (heap-allocated per-player state) |
| 0x745B78 | `B0 A8 6C 00` | another pointer |

**Xrefs to 0x745B70: only 4, all from `sub_526960`** — and `sub_526960`'s decomp does not visibly reference 0x745B70 either (Hex-Rays may have abstracted it into a `dword_XXX` symbol, OR the xrefs are misattributed). Either way: the structure is allocated but **not consumed** during normal gameplay. Diagnosis from v2-plan audit ("TRAP not reuse") is correct.

### 2. The active path: `sub_5CB310("ControlMapper", 0)` → `sub_5CB160(slot)` → ControlMapper

Sample caller — `sub_44E520` (an "is button 2 currently pressed?" gate):

```asm
sub_44E520:
    mov     ecx, [ecx+4]
    push    0                       ; sub_5CB310 a2 (UNUSED — see below)
    push    6A6EE0h                 ; "ControlMapper"
    add     ecx, 0CCCh              ; context object (?)
    call    sub_5CB310              ; eax = registry slot
    mov     ecx, eax
    call    sub_5CB160              ; eax = ControlMapper instance
    mov     edx, [eax]              ; edx = ControlMapper vtable
    push    2                       ; button id 2
    mov     ecx, eax                ; this = ControlMapper
    call    dword ptr [edx+14h]     ; vtable[5] = IsPressed(button_id) -> bool
    test    al, al
    setnz   al
    retn
```

Decomp of `sub_5CB310`:
```c
int __stdcall sub_5CB310(const char *a1, int a2) {
    int v2 = sub_5D3730(a1, strlen(a1));   // registry hashtable lookup
    return v2 ? *(_DWORD *)v2 : 0;
}
```

The `a2` parameter is **unused**. Earlier prose ("hardcoded player_idx=0 everywhere") was misleading — it's not a player index, it's just a stack-passed argument the function never reads. Sibling `sub_5CB350(name)` is the single-arg variant. Both funnel through `sub_5D3730`.

### 3. ControlMapper is a singleton — confirmed by 60 immediate-32 hits

`find type=immediate target=0x6A6EE0` returns **60 distinct code addresses**, every one of them pushing the same `"ControlMapper"` string pointer onto the stack right before a `call sub_5CB310` or `call sub_5CB350`. There is no other ControlMapper instance name (the 4 other `"ControlMapper"` string literals at 0x6a9347/0x6a93c1/0x6bfe59 etc. have **zero xrefs** — dead).

Therefore: one ControlMapper, queried 60 places, no per-player parameter anywhere in the lookup chain.

### 4. ControlMapper vtable[5] sample: `(this, button_id) -> bool`

Two independent sites confirm the same signature:

- `sub_44E520` @ 0x44E542: `push 2; call [edx+14h]`
- `sub_42E860` @ 0x42E8AA: `push 7; call [edx+14h]`

Both pass an integer button id and use `al` (zero/non-zero) — boolean return. No second argument. The implicit `this` is the ControlMapper instance.

### 5. The 3 ControlMapper string-xrefs IDA found are RUMBLE callers

IDA reported only 3 data xrefs to the `"ControlMapper"` string at 0x6A6EE0:

| Fn | Role |
|---|---|
| `sub_5B8D80` | Distance-attenuated camera-shake + rumble pulse via `sub_56E840(slot, intensity\|flags, duration)` |
| `sub_5F2440` | Per-frame rumble decay (`sub_56E880` query, `sub_56E840` apply) |
| `sub_5F2F50` | Camera/state reset including rumble-disable (`sub_56E860(1)`) |

All 3 are rumble/haptic, NOT button polling. IDA missed the other 57 sites because they encode the string as `push 6A6EE0h` (immediate) rather than as a tagged data ref. The full caller list needs `find type=immediate`, not `xrefs_to`.

### 6. `sub_579BF0` is NOT the protagonist input bind

Earlier `coop-phase-0b-breadcrumb-trail-2026-05-10.md` speculated `sub_579BF0(-1, 0.0)` was where the protagonist gets bound to input. Decomp shows it's a small state-reset function:

```c
char __thiscall sub_579BF0(_DWORD *this, int a2, float a3) {
    // zero out fields at this+29..36
    // set 1.0 floats at this+28, this+32, this+36
    result = sub_5799D0(a2, v4, v6);
    if (result) {
        if (fabs(a3) > fabs(v5)) v5 = a3;
        sub_5798D0(v4, v6);
        return 1;
    }
    return result;
}
```

This is a transform/quat/curve reset, not an input binding. The breadcrumb claim was wrong — the protagonist does NOT bind to a per-player input source on construction. The protagonist (like every other consumer) just queries the global ControlMapper singleton when it needs input.

## Implications for Phase 2 design

### Option A — Registry-level proxy (recommended)

Hook `sub_5D3730(name, len)`. When `name == "ControlMapper"`, return a slot that points to a proxy ControlMapper. The proxy's vtable methods consult a "current player" context (thread-local or per-frame globally set) and forward to one of N real input sources.

**Pros:**
- One hook point — clean, easy to reason about.
- Doesn't disturb the rest of the registry.
- All 60 callers transparently route through the proxy.

**Cons:**
- Need to set "current player" context correctly before each per-player code path runs. Default = player 0; explicitly set to player 1 inside the player-2 entity's tick wrapper.

### Option B — Vtable-level patch

Get the real ControlMapper's vtable address (once, at first use), allocate a copy of it, replace each entry with a thunk that resolves player context and forwards. Then overwrite `ControlMapper->vtable` with the patched copy.

**Pros:**
- Even more surgical — leaves the registry untouched.
- Same context-resolution logic as Option A.

**Cons:**
- Have to enumerate all vtable slots actually used (no static signature; needs runtime probe).
- Some slots may be non-input methods (e.g., scheme setters) — must pass-through cleanly.

### Player-context source

Both options need a "current_player_idx" signal. Cleanest: hook the per-entity update path used by Wilbur/player tick (the entity tick loop walks the linked list at `dword_724DE4`), and set the context to the entity's player_idx (a property on the entity) for the duration of its tick. Outside player ticks, context defaults to 0.

For the player_idx-on-entity field: phase-0c findings noted that `.sx` scripts already address actors by player index — but a C++-side `player_idx` field on Wilbur/protagonist instances may not exist yet and may need to be added in Phase 2.

### Estimate

| Item | Estimate |
|---|---|
| Registry proxy + minimal context | 1.0 wk |
| Forward all observed vtable slots | 0.5-1.0 wk (probe-driven; depends on slot count) |
| Per-player context wiring around entity tick | 1.0-1.5 wk |
| Live test + iteration on context-boundary bugs | 0.5-1.0 wk |
| **Total Phase 2** | **~3-4 wk** |

This matches the v2 plan's restored ~4wk estimate (after F2 retraction).

## What this does NOT do

- It does NOT solve where P2 input comes from. The dinput_hook layer captures one device; a second physical device (gamepad-2 or virtual K/M-source-from-network) needs separate provisioning. That's a Phase 1d / Phase 3 problem.
- It does NOT solve UI input. Menus, dialogue, HUD — those should remain P1-only by default; proxy returns player 0's input when called outside an entity tick.
- It does NOT solve the rumble case (sub_56E840). Rumble is identified by the gamepad slot in arg1, not by ControlMapper context. Per-player rumble works naturally if the rumble call passes the correct slot.

## Files referenced

- `0x6A6EE0` — `"ControlMapper"` string literal (the only one with active xrefs)
- `sub_5CB310` @ 0x5CB310 — registry lookup with unused 2nd arg
- `sub_5CB350` @ 0x5CB350 — registry lookup, single arg
- `sub_5CB160` @ 0x5CB160 — lazy-init slot dereferencer
- `sub_5D3730` — underlying hashtable lookup (NOT yet decompiled in detail — would be the optimal hook point)
- `sub_44E520` @ 0x44E520 — exemplar input-query caller (push button_id; call vtable[5])
- `sub_42E860` @ 0x42E860 — larger consumer with multiple ControlMapper queries in one function

## Registry internals (deep dive — for Phase 2 implementation)

### Calling convention reality

Disasm of `sub_5CB310`:
```asm
5cb310  push    esi
5cb311  mov     esi, [esp+4+arg_0]   ; name
5cb315  ...strlen loop...
5cb329  push    eax                  ; len
5cb32a  push    esi                  ; name
5cb32b  add     ecx, 8               ; this = outer + 0x8
5cb32e  call    sub_5D3730
```

So `sub_5CB310` is actually `__thiscall(outer_registry, name, unused)` — the **implicit `this` is the registry container**, and its hashtable lives at offset `+0x8`. Hex-Rays incorrectly inferred `__stdcall` because the function doesn't itself reference `this` (it just forwards `ecx + 8` to `sub_5D3730`). Callers like `sub_44E520` load the outer registry into ecx before the call:

```asm
44e520  mov     ecx, [ecx+4]      ; entity->something
44e523  push    0
44e525  push    6A6EE0h           ; "ControlMapper"
44e52a  add     ecx, 0CCCh        ; registry = (entity->something) + 0xCCC
44e530  call    sub_5CB310
```

So **each caller threads a registry pointer through ECX**. Whether all consumers hit the *same* registry depends on whether `(entity->something) + 0xCCC` aliases to a shared structure across entities — likely yes for a singleton ControlMapper, but worth confirming at runtime.

### `sub_5D3730` hashtable layout

`__thiscall(hashtable*, key_bytes, key_len) → entry_value_ptr`:

```c
if (!this[7]) return 0;                                       // not initialized
v5 = sub_4369C0(key, key_len);                                // hash
v6 = (v5 & this[10]) < this[9]
       ? v5 & ((2 * this[10]) | 1)
       : v5 & this[10];
node = ((dword**)this[4])[v6 >> this[1]][v6 & this[2]];      // 2-level bucket array
while (node) {
    if (node[1] == v5 && node[3] == key_len
        && (!node[2] || !memcmp(key, node[2], key_len)))
        return node + 4;                                      // value area
    node = node[0];                                           // collision chain
}
return 0;
```

Hashtable fields:
- `[+0x04]` = bucket array (pointer to array of pointers to bucket sub-arrays)
- `[+0x08]` = bucket-sub shift (for bucket index)
- `[+0x10]` = bucket-sub mask
- `[+0x1C]` = initialized flag
- `[+0x24]` = current size threshold
- `[+0x28]` = total mask

Node layout:
- `[+0x00]` = next (linked list of collisions)
- `[+0x04]` = full hash
- `[+0x08]` = key pointer (or 0 for hash-only)
- `[+0x0C]` = key length
- `[+0x10..]` = value (returned to caller as `node + 4` = `&value`)

### Registry slot record layout (28 bytes; what `sub_5CB160` reads)

Union of writer (`sub_5CB420`) and reader (`sub_5CB160`):

| Offset | Size | Field | Notes |
|---|---|---|---|
| +0x00 | dword | type_id | caller-specified; passed as 1st arg to lazy-init callback |
| +0x04 | byte | needs-init flag | starts at 1; cleared after callback runs successfully |
| +0x05..0x07 | — | padding | (low byte of slot[1] overlaps the flag) |
| +0x08 | dword | lazy-init callback | `int (__cdecl *)(type_id, slot*)` — populates +0x0C |
| +0x0C | dword | storage pointer | for kind==5, this is a pointer-to-pointer; deref → resource |
| +0x10 | dword | kind | 0/1/5/6 → 4-byte; 2 → 12-byte; 3 → 1-byte; 4 → 36-byte alloc |
| +0x14 | dword | name intern id | `sub_5D3E30(name)` |
| +0x18 | byte | inline-storage flag | 1 if storage was auto-allocated (a5==0 in registrar call) |

Reader logic (`sub_5CB160`):
```c
if (slot+4 byte set) {
    if (slot[2]) {                       // callback exists
        if (slot[2](slot[0], slot))      // (type_id, slot) — true = init OK
            slot+4 byte = 0;
    }
}
if (slot[4] == 5)                        // kind 5 = "resource"
    return *(slot[3]);                   // deref storage to get instance ptr
return 0;
```

The kind enum:
- `5` = resource (pointer-to-pointer); `sub_5CB160` returns `*(slot[3])`
- `0/1` = 4-byte primitive (cvar?); reads/writes 4 bytes inline at allocated storage
- `2` = 12-byte (probably vec3 cvar)
- `3` = 1-byte (bool)
- `4` = 36-byte (matrix?)
- `6` = 4-byte zero-initialized

ControlMapper is kind=5 (resource pointer-to-pointer).

### Why the ControlMapper registrar wasn't found via immediate-32

Of 60 immediate `push 6A6EE0h` sites, **none** are immediately followed by `call sub_5CB420` — all hit `sub_5CB310`/`sub_5CB350` (readers). The registrar either:
1. Loads the name string from a variable (`mov eax, ds:g_cm_name_ptr; push eax`) — would not show up in immediate-32 search.
2. Registers ControlMapper via a different mechanism (constructor-side, not the generic registry).
3. Is reached only at runtime through a script command that takes the name as a parameter.

For Phase 2 this is non-blocking — we don't need to know the install path to install our proxy.

## Phase 2 hook design (concrete)

### Recommended: hook `sub_5CB310` + `sub_5CB350` (the two read-entry points)

Reasoning: these are named, small (~50 bytes each), thiscall functions with simple signatures and only ONE underlying purpose (registry read). Hooking both catches every consumer call path. The hashtable-level intercept at `sub_5D3730` is broader but also catches non-ControlMapper reads (cvars, scripts, etc.) — more work, no benefit for input separation.

### Hook signature (PRE-callback style)

```c
// Pre-hook: intercept before original runs.
// Return non-zero to short-circuit with our slot; zero to fall through to original.
int __cdecl pre_lookup(const char* name, _DWORD** out_replacement_slot) {
    if (!is_control_mapper_name(name)) return 0;     // memcmp against "ControlMapper"
    int player_idx = mtr::input::current_player_context();
    *out_replacement_slot = mtr::input::proxy_slot_for_player(player_idx);
    return 1;
}
```

Implementation: naked stub PRE/POST hook (the pattern documented in `project_naked_stub_pre_post_hook_2026-05-09.md`) — preserve EAX/ECX/EDX, save the name string pointer from the stack, dispatch to the pre-callback, and either jump past the original or fall through to the trampoline.

### Current-player-context source

Per-frame context, set/cleared around the player entity's tick:

```c
// In a hook on whatever calls each player entity's tick
void player_entity_tick_pre(EntityBase* e) {
    int pid = e->player_idx;          // new field; default 0 for player 1
    mtr::input::push_player_context(pid);
}
void player_entity_tick_post() {
    mtr::input::pop_player_context();
}
```

Default context = 0 (player 1). UI, HUD, dialogue, debug — all stay as player 1 unless inside a player-2 entity's tick.

### Proxy ControlMapper instances

Two real ControlMapper instances (or N for multi-player):
- `g_control_mapper_p1` = the engine's existing one (captured on first read)
- `g_control_mapper_p2` = a fresh instance (constructed via whatever path the engine uses for the original — TBD via Phase 1d runtime probe)

Each player's instance reads its OWN DI device or network-fed input source. The dinput_hook layer (already shipped — see `project_dinput_buffer_injection_2026-05-09.md`) gives us a buffer injection primitive for P2 keyboard.

### Phase 2 work items (concrete)

1. **Runtime probe**: log every `sub_5CB310("ControlMapper", _)` call site with (ECX value, ret addr, caller fn). Confirms all consumers hit the same registry container — or reveals exceptions. (1-2 days)
2. **Vtable surface probe**: log every method invoked on the result. Outputs the set of vtable slots the proxy must implement. (1-2 days)
3. **Singleton-capture**: on first probe-observed read, capture the ControlMapper instance + vtable address. (0.5 day)
4. **Construct P2 instance**: figure out the engine's own ControlMapper construction path (likely a class ctor or a `*Create*` symbol) and call it. (1 wk — wildcard, depends on discovery)
5. **Proxy + thunk vtable**: generate a forwarding vtable that dispatches per current_player_context. (3-5 days)
6. **Hook the two read entry points** to return proxy slot for "ControlMapper". (2-3 days)
7. **Wrap player entity tick** to set/clear context. (3-5 days — depends on finding the right tick site)
8. **Live test + iterate** on context-boundary bugs (UI/HUD bleed, mini-game scenes, scripted cutscenes). (1 wk)

**Total: ~3-4 weeks**, matching the v2 plan's restored Phase 2 estimate.

### Risk: kind!=5 / null slot fallback

`sub_5CB160` returns 0 for slot.kind != 5. Our proxy slot MUST have kind=5 OR we short-circuit `sub_5CB160` too. The cleanest path: install a real-shape proxy slot (with kind=5, valid callback, storage pointer-to-pointer-to-our-proxy-instance) and let `sub_5CB160` work unmodified. The proxy slot lives entirely in mod memory; the engine just sees a normal kind=5 record.

## Live runtime probe results (2026-05-11 — `controlmapper_probe`)

A runtime probe was shipped (`src/mtr-asi/src/coop/controlmapper_probe.cpp`) that hooks both `sub_5CB310` and `sub_5CB350` and logs every lookup whose name matches "ControlMapper". Run against two scenarios:

### Scenario: `boot-to-main-menu` (1.7s, 301 frames)

**Zero "ControlMapper" lookups.** Menu input does NOT go through the ControlMapper. Likely path is direct DirectInput polling or the widget-callback dispatcher. Confirms ControlMapper is gameplay-specific.

### Scenario: `load-save-1-show-ingame` (10.0s, 1873 frames)

**~1500+ lookups in 10s (~150/sec)** — ControlMapper IS the hot path in gameplay.

| Metric | Value | Interpretation |
|---|---|---|
| Total lookups | 1500+ | very hot |
| Distinct `outer` registries | **2** (0x0F04DCAC, 0x0EFE46FC) | two different holders chain to the same slot |
| Distinct slot pointers | **1** (0x0F0220E0) | single slot — confirms singleton |
| Distinct instance pointers | **1** (0x0F022120) | single ControlMapper instance |
| Distinct vtable pointers | **1** (0x006A639C) | single vtable, static address |
| Slots with kind != 5 | 0 | all healthy |

Two outer registries chain to the same slot record, which dereferences to one instance with one vtable. **Singleton hypothesis confirmed live.**

### Boot lazy-init sequence (3 first calls captured)

| # | Via | RA | Outer | Slot | Kind | Storage | Instance | Vtable |
|---|---|---|---|---|---|---|---|---|
| 1 | sub_5CB350 | 0x005F2F81 (rumble decay) | 0x0F04DCAC | **0** (null) | - | - | - | - |
| 2 | sub_5CB310 | 0x0051F5A3 | 0x0F04DCAC | 0x0F0220E0 | 5 | 0x0F022060 | **0** (not yet init) | - |
| 3 | sub_5CB350 | 0x0051EF82 | 0x0F04DCAC | 0x0F0220E0 | 5 | 0x0F022060 | 0x0F022120 | **0x006A639C** |

Three-phase lazy init: (1) first lookup returns null — slot not registered yet; (2) second lookup returns the slot but instance is null (lazy-init callback hasn't run); (3) third lookup returns the fully-initialized slot with instance + vtable. **Our proxy must handle the null-instance window gracefully**, OR pre-register the proxy eagerly during the install sequence.

### ControlMapper vtable @ 0x006A639C (static .rdata; 13 slots — full characterization)

Captured via `get_bytes` after probe identified the address. Vtable extends 52 bytes (13 dwords) then terminates at the next class's `"Wilbur\0"` literal at 0x6A63D8. All 13 slots decompiled 2026-05-11 (late session); IDB names applied.

| Slot | VA | IDB name | Signature | Role |
|---|---|---|---|---|
| 0 | 0x006914F0 | `ControlMapper_dtor_vt0` | `void __stdcall(this, uint should_free)` | virtual destructor; `should_free & 1` → calls `operator delete (sub_5832F0)` |
| 1 | 0x004F92A0 | `nullsub_1` | `void __stdcall(int)` | true no-op (inherited base method, never overridden) |
| 2 | 0x0059AAF0 | (single `RET` byte) | `void __cdecl()` | true no-op stub (1 byte: `C3`) |
| 3 | 0x0056EEC0 | `ControlMapper_Tick_vt3` | `void __thiscall(this)` | per-frame tick: shifts 18-byte ring at `this+0xF0` down by 1 (curr→prev), then jumps `sub_56ECE0` (Update) |
| 4 | 0x0056E8D0 | `ControlMapper_GetAnalog_vt4` | `float __thiscall(this, int button_id)` | reads `dev->axis_table[idx*4+48]`; multiplies by `this[+88+4*button_id]` scale factor; gated by `this+336` & `this+8` enable bytes |
| 5 | **0x0056E940** | **`ControlMapper_WasJustPressed_vt5`** | `bool __thiscall(this, int button_id)` | **edge-detect**: `prev_byte == 0 && curr_byte == 1` at `(this+4)+2*button_id+12..13`; previously called "IsPressed" — corrected |
| 6 | 0x0056E9C0 | `ControlMapper_WasJustReleased_vt6` | `bool __thiscall(this, int button_id)` | edge-detect inverse: `prev == 1 && curr == 0` (release-edge) |
| 7 | 0x0056EA40 | `ControlMapper_IsHeldOrAux_vt7` | `bool __thiscall(this, int button_id)` | `sub_5707C0(button_id, 1) \|\| this[2*button_id+241]` — held / aux flag query |
| 8 | 0x00401000 | `ControlMapper_LoadButtonProfile_vt8` | `void __stdcall(this, int profile_id)` | 13-case dispatcher calling `sub_56E710` (= `ControlMapper_BindButton`) to set up button mapping. Default case recurses via `vtable[8]`. Each case binds 1-14 pairs `(dst_button, src_button)` |
| 9 | 0x00401270 | `ControlMapper_LoadHatProfile_vt9` | `void __stdcall(this, int profile_id)` | 13-case dispatcher calling `sub_56E8A0` (= `ControlMapper_BindHat`) with 3-arg form `(0, dst, src)`. Hat / d-pad equivalent of slot 8 |
| 10 | 0x0056EB60 | `ControlMapper_GetMousePos_vt10` | `void __thiscall(this, _DWORD* out2)` | copies `(this+4)[160]` + `(this+4)[164]` into `out2[0..1]`; zeros both if disabled — likely current `(mouse_x, mouse_y)` or `(active_button_state, frame_id)` |
| 11 | 0x0056EBC0 | `ControlMapper_GetFlagA8_vt11` | `bool __thiscall(this)` | returns byte at `(this+4)+0xA8`; gated by enable bits — likely "device alive" or "input frame valid" |
| 12 | 0x0056EE50 | `ControlMapper_GetVec3_vt12` | `void __thiscall(this, _DWORD* out3, int idx)` | copies 12 bytes from `(this+4)+124+12*idx` into `out3[0..2]`; fallback `(0, -1.0f, 0)` (sentinel) if disabled — vec3 array accessor (axis triple? position?) |

**Helper non-vtable methods** (renamed):
- `0x0056E710` → `ControlMapper_BindButton(this, dst, src)` — actual button-binding writer; called by slot 8 (button profile loader)
- `0x0056E8A0` → `ControlMapper_BindHat(this, p1, dst, src)` — hat / d-pad binding writer; called by slot 9

**Edge-detect ring layout** (decoded from slots 4/5/6/7):
- `this+4` = pointer to the input device state object (call it `dev`)
- `dev+0xA8` = device alive flag (slot 11)
- `dev+160..164` = mouse position / pair value (slot 10)
- `dev+12 + 2*button_id` = `[prev_pressed_byte, curr_pressed_byte]` — 2-byte rolling history per button
- `dev+48 + 4*axis_idx` = analog axis float
- `this+8` = ControlMapper-level enabled byte
- `this+0xF0..0x102` = 18-byte ring (shifted by slot 3 each tick)
- `this+0x150` = "main enabled" flag (also referenced as `this+336`)
- `this+88+4*button_id` = per-button analog scale
- `this+241+2*button_id` = per-button aux flag (held override)
- `this+164+4*button_id` = button → device-button mapping index

**Slot kinds for proxy implementation**:
- **Stateless query** (4 slots): 5, 6, 7, 11 — read state, return bool — easy to proxy per-player
- **Stateless query w/ output buffer** (3 slots): 4, 10, 12 — same but write to caller's buffer — easy
- **State mutator** (3 slots): 0 (dtor), 3 (Tick), 8 (LoadButtonProfile), 9 (LoadHatProfile) — must be per-instance (each player has its own keymap)
- **No-op inherited** (2 slots): 1, 2 — proxy can pass-through to nullsub safely
- **Dtor** (1 slot): 0 — proxy needs careful handling (don't delete the real instance via the proxy path)

For Phase 2: only slots 3-12 need per-player divergence. Slots 0/1/2 can be left alone (1/2 are no-ops; 0 is dtor — proxy never invokes it).

### Phase B live probe: per-slot call-distribution from instrumented vtable (2026-05-11)

A second probe was shipped (`src/mtr-asi/src/coop/controlmapper_vtable_probe.cpp`) that:

1. Allocates a 13-dword proxy vtable in mod heap.
2. Copies the original vtable contents for slots 0/1/2 (pass-through).
3. Installs naked-stub logging thunks at proxy slots 3..12 (each: `push imm32; jmp dispatcher`). The dispatcher preserves ECX/EDX, calls `record_call(slot_idx, caller_ra, arg0)`, then jumps to the original method with the engine-supplied ABI intact.
4. Rewrites `instance[0]` to point at the proxy (NOT the static .rdata vtable — instance-pointer rewrite only). Reversible.
5. Gated on cmdline flag `-mtrasi-cmvt-probe-arm`.

Run against `load-save-1-show-ingame` scenario (10.2s total, ~1.1s post-arm). Test PASSED — vtable rewrite produced zero behavior change.

**Per-slot results (1.1s armed runtime, 4,348 total calls captured)**:

| Slot | Method | Calls | Rate (call/s) | Distinct callers (RAs) | Argument range |
|---|---|---|---|---|---|
| 0 | dtor | 0 | — | — | — |
| 1 | nullsub | 0 | — | — | — |
| 2 | RET stub | 0 | — | — | — |
| 3 | `Tick` | 182 | 167 | **1** (`0x0056F361`) | unused (1st stack arg = ptr-junk) |
| 4 | `GetAnalog` | 1088 | 1000 | 6 | button_id ∈ {2,3} |
| 5 | `WasJustPressed` | **2355** | **2168** | 11 | button_id ∈ {0..13} |
| 6 | `WasJustReleased` | 362 | 333 | 2 | button_id = 4,9 |
| 7 | `IsHeldOrAux` | 361 | 332 | 2 | button_id = 4,9 |
| 8 | `LoadButtonProfile` | **0** | — | — | — |
| 9 | `LoadHatProfile` | **0** | — | — | — |
| 10 | `GetMousePos` | **0** | — | — | — |
| 11 | `GetFlagA8` | **0** | — | — | — |
| 12 | `GetVec3` | **0** | — | — | — |

**Distinct callers per active slot** (full RAs captured):

```
slot 3 (Tick):           0x0056F361   (single tick site — input-module per-frame Update)
slot 4 (GetAnalog):      0x00404264 0x0040428C 0x005454F5 0x00545502 0x0052DDA0 0x0052DDAD
slot 5 (WasJustPressed): 0x0040A6FA 0x004985ED 0x0049861C 0x00459D8C 0x0044E545 0x0052D516
                         0x0048A29A 0x0048A375 0x0048993D 0x004540E2 0x0048ABEC
slot 6 (WasJustReleased): 0x0048A22F 0x0048A39A
slot 7 (IsHeldOrAux):     0x0048A3BF 0x0048AC0F
```

Total distinct call sites observed: 22 (across all 5 active slots). Recall the prior static-RE found 60 immediate-32 references to `"ControlMapper"` — those are the upstream registry-lookup sites, each of which makes several vtable calls. The 22 RAs above are the actual vtable-invocation sites in the load-save scenario.

**Phase 2 design simplification (post-Phase B probe)**:

1. **Only 5 slots need per-player divergence**: 3, 4, 5, 6, 7. Slots 0/1/2 stay pass-through (no-ops + dtor). Slots 8/9/10/11/12 also stay pass-through — they're never invoked in normal gameplay.

2. **Keymap is shared, not per-player**: slot 8 (`LoadButtonProfile`) and slot 9 (`LoadHatProfile`) — the only slots that mutate the per-instance keymap — fire ZERO times during gameplay. They're called at boot / options-menu only, outside our hot path. Conclusion: **both players can share the same keymap**. We do not need to construct a second ControlMapper instance with its own keymap.

3. **The actual per-player state is the `dev` struct at `(this+4)`**: slots 4/5/6/7/11 all read `dev[button_offset]`. Slot 3 (Tick) shifts the state ring inside `dev`. To split inputs, allocate a second `dev` structure for player 2; let the proxy thunks pick `dev` based on current-player context; slot 3 ticks both `dev`s in sequence.

4. **Slot 5 dominates hotpath**: 2168 calls/sec (~36 calls/frame at 60fps). Thunk must be cheap. The current 6-push/3-arg-call dispatcher is ~50 cycles per call; even at 11 button-press queries per player per frame × 2 players × 60fps = 1320 calls/sec — well within budget.

5. **Phase 2 work-item count drops further**: instead of building a full P2 ControlMapper instance (the prior wildcard work-item, est. 1wk), allocate just the `dev` sub-struct. Inferred fields from slot decomps: prev/curr ring (2 bytes per button × N buttons), analog scale (4 bytes per button), button→device map (4 bytes per button), main enabled byte, device-alive byte at +0xA8, mouse pos at +160/+164. Estimated total: 256-512 bytes, doable in ~2-3 days.

**Phase 2 revised estimate (post-Phase B)**: ~**1.5-2.5 wk** (was 2.5-3.5wk pre-Phase B; was ~4wk pre-Phase A/probe). Most of the remaining work is per-player context wiring around the player entity tick (1-1.5wk) and live testing.

**Probe files**:
- `src/mtr-asi/include/mtr/controlmapper_vtable_probe.h` — public API
- `src/mtr-asi/src/coop/controlmapper_vtable_probe.cpp` — implementation (naked thunks + dispatcher)
- Wired into `dllmain.cpp` + `controlmapper_probe.cpp::capture` (arm-on-first-observation)
- Test harness `hard_kill_self()` calls `dump_to_log("on-hard-kill")` before TerminateProcess
- Build: 688,128 bytes (+1024 over Phase A; +4096 over pre-probe)

### Ctor + layout audit (2026-05-11 very late)

Found the ControlMapper constructor chain via `find type=immediate target=0x006A639C` (1 hit):

```
ControlMapper_new_wrap @ 0x004ABBA1     ; new + ctor wrapper (if non-null: ctor)
└─ ControlMapper_ctor @ 0x004013E0      ; derived ctor — overrides vtable + loads default profiles
   └─ ControlMapper_base_ctor @ 0x0056EBF0   ; base ctor — initializes all per-instance fields
```

**ControlMapper_ctor (0x4013E0) decomp**:
```c
ControlMapper_ctor(this) {
    ControlMapper_base_ctor(this, "Wilbur");        // base init, writes vt=0x6A6368 (base vtable)
    *this = 0x006A639C;                              // override to ControlMapper vtable
    (vtable[8])(this, 0);                            // LoadButtonProfile(profile_id=0)
    (vtable[9])(this, 0);                            // LoadHatProfile(profile_id=0)
    *(float*)(this+0x154) = 0.5f;
    *(float*)(this+0x158) = 0.5f;                    // two float fields (likely deadzone/threshold pair)
    return this;
}
```

**ControlMapper_base_ctor (0x56EBF0) decomp** (extracted field-by-field):
```c
ControlMapper_base_ctor(this, name) {
    *this           = 0x006A6368;          // base vtable
    this[+8]        = 1;                    // enable byte (slot 4/5/6/7/11/12 check this)
    memcpy(this+304, name, strlen(name)+1); // name copy at this+0x130 ("Wilbur\0")
    this[+4]        = 0;                    // *** dev pointer = NULL — set externally later
    this[+336]      = 0;                    // main enabled byte (this+0x150; gated again)
    // Zero analog axis MAPPING at this+12+4*N (N=0..18, 76 bytes)
    // Init analog axis SCALE at this+88+4*N to 1.0f (N=0..18, 76 bytes)
    // Zero button MAPPING at this+164+4*N (N=0..18, 76 bytes)
    // Zero aux-ring at this+240+2*N (N=0..17, 36 bytes) — this IS the Tick ring
    this[+276]      = 0;                    // "tick-active" byte (Tick body gates on this+276)
    return this;
}
```

**`this` (ControlMapper) layout — 352+ bytes (final size TBD; "Wilbur" name fits at this+304..this+310)**:

| Offset | Size | Field | Source |
|---|---|---|---|
| +0x000 | 4 | vtable_ptr | ctor `*this = 0x6A639C` |
| +0x004 | 4 | **dev pointer** | base ctor sets =0; written externally later |
| +0x008 | 1 | enable_byte_a | base ctor `=1`; checked by slots 4/5/6/7/11/12 |
| +0x009 | 3 | padding |  |
| +0x00C | 76 | analog_axis_mapping[19] | this+12+4*N; zeroed; read by slot 4 (`this[12+4*a2]`) |
| +0x058 | 76 | analog_axis_scale[19] | this+88+4*N; init 1.0f; read by slot 4 (return value × this[88+4*a2]) |
| +0x0A4 | 76 | button_mapping[19] | this+164+4*N; zeroed; read by slots 5/6/7 (`this[164+4*a2]`) |
| +0x0F0 | 36 | aux_ring[18] (2 bytes each) | this+240+2*N; written by Tick body (sub_56ECE0); shifted by slot 3 (Tick); byte[2N]=prev (slot read by slot 6 fallback), byte[2N+1]=curr (slot 7 reads `this+241+2*v3`) |
| +0x114 | 1 | tick_active_byte | this+276 = 0 in base ctor; gates Tick body; flipped on by "Enable" path |
| +0x115..0x118 | 4 | float threshold | this+280; deadzone for axis→edge detection in Tick body |
| +0x128 | 8 | ? |  |
| +0x130 | ~16 | name | this+304; name string ("Wilbur\0...") copied here |
| +0x150 | 1 | enable_byte_b | this+336 = 0 in base ctor; second-level enable; checked by all slots |
| +0x154 | 4 | float ? | this+340 = 0.5f set in derived ctor |
| +0x158 | 4 | float ? | this+344 = 0.5f set in derived ctor |

Estimated total `this` size: ~360 bytes (0x168).

**ControlMapper_Tick_body (0x56ECE0) — Tick's tail-call target — also clarifies aux-ring writes**:
```c
ControlMapper_Tick_body(this) {
    if (!this[+276] || !this[+336]) {
        this[+243] = this[+245] = this[+247] = this[+249] = 0;  // clear 4 direction bits
        return;
    }
    dev = this[+4];
    if (dev) {
        v8 = dev[+52]; if (dev[+180] && v8==0) v8 = sub_41A620(1, 0);  // axis 1 (left-stick X)
        v9 = dev[+56]; if (dev[+180] && v9==0) v9 = sub_41A620(2, 0);  // axis 2 (left-stick Y)
    }
    threshold = *(float*)(this+280);
    this[+247] = (v8 > threshold);              // button 3 curr-byte: axis1 positive edge
    this[+243] = (v8 < -threshold);             // button 1 curr-byte: axis1 negative edge
    this[+249] = (v9 > threshold);              // button 4 curr-byte: axis2 positive edge
    this[+245] = (v9 < -threshold);             // button 2 curr-byte: axis2 negative edge
    sub_56EAA0(1); sub_56EAA0(3); sub_56EAA0(4); sub_56EAA0(2);  // 4 follow-up processors
}
```

So the aux ring is the **engine's own per-frame edge-detect cache** for the d-pad direction buttons (button_id 1/2/3/4), computed from raw analog axes in `dev`. Slot 3 shifts curr→prev; Tick_body writes new curr. Slot 7 (`IsHeldOrAux`) reads `this[241+2*N]` = the curr byte.

**`dev` (input device state, pointed to by this+4) — extracted from slot 4/5/6/7/10/11 reads**:

| Offset | Size | Field | Source |
|---|---|---|---|
| +0x000 | 12 | ? | (likely device-class header) |
| +0x00C | 36 | button_ring[18] (2 bytes each) | dev+12+2*N; slot 5 reads `dev[12+2N]` (prev byte=0) & `dev[13+2N]` (curr byte=1) — engine fills this from raw DI buffer |
| +0x030 | ? | analog_axis_values[?] | dev+48+4*N; slot 4 reads `dev[48+4*v4]` as float; ControlMapper_Tick_body reads `dev[52]` (axis 1) and `dev[56]` (axis 2) |
| +0x0A0 | 8 | mouse_xy or pair | dev[160], dev[164]; slot 10 returns these as 2 dwords |
| +0x0A8 | 1 | device_alive_byte | dev+0xA8; slot 11 returns this |
| +0x0B4 | 4 | mode_flag | dev+180; ≠0 enables "alternate mode" — fallback to sub_41A620/41A5E0/5707C0/5707C0 for slots 4/5/6/7 |

Estimated total `dev` size: 256+ bytes (most likely 0x100-0x200 depending on axis count and trailing fields).

**`dev` allocation site — STILL TBD**:
- Base ctor sets `this+4 = 0`. Some external function (likely "AttachDevice" or "BindInputSource") writes `this+4 = <ptr>`.
- 60-immediate-32 search for the registrar `sub_5CB420("ControlMapper", ...)` returned 0 hits — registrar uses indirect string ref.
- Searching for `mov [reg+4], <ptr>` patterns is too broad without knowing the register.
- **Recommended path: add a runtime probe** that polls `*(instance+4)` after the existing `controlmapper_probe` captures the instance, and logs the first non-null read + caller RA (via stack walk if possible). One-shot logging.

**Revised Phase 2 work-item estimate (post-layout-audit)**:

| Item | Pre-audit | Post-audit |
|---|---|---|
| Per-player divergence on 5 slots | 3-5 days | 3-5 days |
| Allocate `dev_P2` | 2-3 days | 2-3 days (need size finalized first) |
| **Also: copy `this+240..this+304` per player** | not planned | **2 days** (newly discovered) |
| Per-player context wiring around player entity tick | 1-1.5 wk | 1-1.5 wk |
| Find dev allocation site (P2 dev populates path) | 2-3 days | 2-3 days |
| Live test + iterate | 1 wk | 1 wk |
| **Total** | **1.5-2.5 wk** | **~2-3 wk** (slight increase due to ring split insight) |

Net: estimate creeps back from "1.5-2.5wk" to "2-3wk", but still well under the 4wk pre-probe baseline.

### dev struct memory dump + family RE (2026-05-11 very late, continuation)

Extended `controlmapper_probe` to log `*(instance+4)` (dev pointer) the moment it transitions null → non-null + dump 512 bytes of dev memory + the dev vtable. Test PASSED again.

**Live dev capture** (run 2026-05-11, scenario load-save-1):
- `instance = 0x0ED93120`
- `dev     = 0x0F7389E0`  (heap allocated)
- `caller_ra = 0x0051EF82` (in sub_51EE90 — iterates registered subsystems and calls Enable/Disable; dev is already non-null at the FIRST observed registry read for ControlMapper, so dev allocation happens upstream of this RA)

**dev vtable @ 0x006C80B8** (16 slots):
```
vt[0] = 0x00572ED0  (dtor)         vt[8]  = 0x00570C20
vt[1] = 0x00572EC0                  vt[9]  = 0x00432610  (default-impl,
vt[2] = 0x00572180                                         repeated 7 slots)
vt[3] = 0x00452050                  vt[10] = 0x00432610
vt[4] = 0x00432610                  vt[11] = 0x00432610
vt[5] = 0x004F92A0  (nullsub_1 —    vt[12] = 0x00432610
                     shared w/      vt[13] = 0x00432610
                     ControlMapper  vt[14] = 0x00432610
                     vt[1])         vt[15] = 0x00432610
vt[6] = 0x0052D670
vt[7] = 0x005722E0
```

The 7 repeated `0x00432610` slots are an inherited default. Only slots 0-3, 5-8 are real methods.

**dev hex dump (initial state, 512 bytes captured)** — selected interesting fields:
```
dev+0x000: 006C80B8 00000000 FFFFFFFF 00000000  ← vtable, +0x04=0, +0x08=-1
dev+0x040: 80000000 ...                          ← float -0.0 sentinel
dev+0x080: BF800000 ........ ........ BF800000  ← floats -1.0 at +0x80, +0x8C
dev+0x090: ........ ........ BF800000 ........  ← float -1.0 at +0x98
dev+0x0A0: ........ ........ 04000100 ........  ← byte +0xA9 = 1 (device_alive-related)
dev+0x0B0: FFFFFFFF ........ ........ ........  ← -1 sentinel
dev+0x0C0: 00000001 ........ 00000064 0071AD1C  ← +0xC0=1 (?), +0xC8=100 (?), +0xCC=0x71AD1C (fn ptr)
dev+0x0D0: 00000016 00000012 ........ 00000003  ← +0xD0=22 (count?), +0xD4=18 (count?), +0xDC=3
dev+0x0E0: 0000003A 0000003B 0000003C 0000003E  ← scan-code/key-code table starts (~22 entries)
dev+0x0F0: 0000004C 0000004B 00000045 00000047
dev+0x100: 00000049 0000004A 00000024 00000005
dev+0x110: 0000005E 0000005C 00000004 00000063
dev+0x120: 00000010 00000032 00000030 00000050
dev+0x130: 00000030 ........ ........ 2000050C
dev+0x140: ........ <pseudo-random hashes + heap ptrs — looks like hashtable buckets>
dev+0x150: 0F8561B0 0F2C47E0 B12D059B 0F852324
dev+0x160: 00000011 0F852310 00000000 C0D8BCB0
                                       ...
dev+0x1F0: 0F849880 ........ A3221CB6 0F849864
```

The pattern at dev+0x140..end is clearly a **hashtable** (4-dword records: probably `{next_ptr, hash, key_ptr, value_ptr}`). At least 8-12 entries visible in the 256-byte tail of the dump. dev size is therefore > 512 bytes, likely 1KB-2KB depending on bucket capacity.

**Dev family — at least 3 derived classes share base vtable 0x6C8150**:

| Vtable | Ctor | Allocator | Size | Renamed |
|---|---|---|---|---|
| `0x006C8150` | `InputDevice_base_ctor` @ 0x00570820 | — (called by derived ctors) | ~184 bytes for base portion | `InputDevice_base_ctor` |
| `0x006C80B8` | `InputDevice_classA_init_vt6C80B8` @ 0x00524B22 | **TBD** | ≥ 512 bytes (hashtable inside) | **This is OUR observed ControlMapper-attached dev** |
| `0x006C8C40` | `InputDevice_classB_ctor_vt6C8C40_20284b` @ 0x00576A60 | `InputDevice_classB_alloc_site` @ 0x00573A20 (`mtr_alloc(20284)`) | **20,284 bytes** | Large input-map device — likely a keyboard or full controller binding |

`InputDevice_classA_init_vt6C80B8` has NO static xrefs (not as immediate, not as data, not as 4-byte sequence anywhere). It is reached only via an indirect call mechanism (possibly a function-pointer table or an obfuscated jump pattern visible at sub_524B00). Locating its caller via static RE is a wildcard — better path is **a runtime probe that hooks `mtr_alloc` (sub_5832C0) and filters by post-call vtable=0x6C80B8 write**.

**ControlMapper `this` size now firmly known**: `sub_4ABB6C` allocates with `push 0x164` → **356 bytes**. Matches the layout audit estimate. Class breakdown:
- this+0x000..0x163 = ControlMapper data (vtable, dev ptr, mappings, aux ring, name, enable bytes, threshold)

### Pragmatic Phase 2 path forward (without complete dev RE)

Given that:
1. dev's exact size is unknown (but ≥ 512 bytes, likely 1-2 KB)
2. dev allocator is hard to find via static RE
3. dev contains an embedded hashtable with possible internal pointers

**Proposed P2 dev approach**:
1. At the moment we observe instance+4 becomes non-null, snapshot dev into a 4 KB heap-allocated buffer (massive over-estimate; cost of slack is negligible).
2. For the per-player routing thunks, swap `instance+4` between P1 dev (original) and P2 dev (our copy) based on `current_player_context`.
3. **Risk**: if dev's hashtable contains pointers to OTHER heap structures (button-binding map, hat-binding map), the memcpy shares them between players. For most reasonable interpretations — these are READ-ONLY shared key-binding tables — this is FINE and even desired. The button-press STATE (the ring at dev+12) is byte-only and the memcpy gives each player their own copy.
4. **Initial test**: after memcpy, swap `instance+4` to P2 dev and verify ZERO behavior change (engine plays as if P1 dev was untouched, since the state evolves identically initially). Then differentiate by feeding different DI inputs into each dev's button-ring.

This avoids the rabbit hole of finding dev's allocator while still providing per-player divergence. Phase 2 step (b3-b4) estimate revises:
- (b3-b4) Allocate dev_P2 via 4KB heap + memcpy: **1 day** (was 2-3 days for "find exact size + allocator")
- Net Phase 2 estimate: returns to **~1.5-2.5wk** (was 2-3wk after audit, when we thought we needed exact size)

**Files added/touched this audit**:
- `src/mtr-asi/src/coop/controlmapper_probe.cpp` — added `DEV ATTACHED` log + 512-byte hex dump + 16-slot vtable dump on first non-null `*(instance+4)` observation
- IDB renames: `ControlMapper_dtor_vt0`, `ControlMapper_Tick_body`, `ControlMapper_ctor`, `ControlMapper_base_ctor`, `ControlMapper_new_wrap`, `InputDevice_base_ctor`, `InputDevice_classA_init_vt6C80B8`, `InputDevice_classB_ctor_vt6C8C40_20284b`, `InputDevice_classB_alloc_site`, `InputDevice_dtor_demote_to_base`

### Phase 2 step (b4) — dev_P2 capture + swap-test SHIPPED + VALIDATED (2026-05-11 very late)

Implementation: `controlmapper_vtable_probe::capture_dev_p2(instance, dev_p1)` allocates a 4KB heap buffer + SEH-guarded memcpy from `dev_p1` (with size fallback 4096→2048→1024→512→256 on fault). Called from `controlmapper_probe::capture` the moment `*(instance+4)` transitions null → non-null. Public API:

```cpp
void capture_dev_p2(uint32_t instance_addr, uint32_t dev_p1_addr);
uint32_t dev_p1_addr();
uint32_t dev_p2_addr();
bool     swap_test_armed();
void     disarm();  // restores instance+4 to dev_p1
```

**Swap-test mechanism** (gated on `-mtrasi-cmvt-swap-test`):
1. capture_dev_p2 runs (memcpy dev_P1 → dev_P2)
2. SEH-guarded write of `instance[+4] = dev_P2_addr`
3. Engine now reads ALL device state from our 4KB memcpy buffer

**Test results (load-save-1-show-ingame scenario, 2026-05-11)**:

| Configuration | Result | Frames | Time | Behavior |
|---|---|---|---|---|
| Unarmed (baseline) | PASS | 1872 | 10.13s | normal |
| `-mtrasi-cmvt-swap-test` | **PASS** | **1871** | **9.86s** | **identical** |

The memcpy was successful at full 4 KB without SEH fault on first try. Captured copy: `dev_p1=0x0F90E9E0 → dev_p2=0x1FAE60E8`. Engine ran 9.86 seconds reading the ENTIRE device state from our heap buffer — zero behavioral difference.

**This validates the Phase 2 (b5) routing-thunk mechanism end-to-end**:
1. 4KB memcpy captures dev's full state (confirmed: no fault, behavior identical).
2. `instance+4` swap is safe (no engine cache/RTTI gotchas; vtable not consulted by engine for sanity).
3. Per-player divergence WILL work via this exact swap mechanism.

**Implications for Phase 2 (b5) — routing thunks**:
- The 5 active thunks (slots 3-7) each pre-call swap `instance+4` to `dev_P2` if `current_player_context == 1`, call original, then post-call restore to `dev_P1`.
- Naked-stub dispatcher needs minor expansion: save+restore instance+4 around the call.
- Slot 3 (Tick) special case: call ORIG twice with different `instance+4` values so both player states tick per frame.
- All other thunk semantics unchanged.

**Phase 2 estimate finalized at ~1.5-2.5wk** (down from 4wk pre-Phase-A, 2.5-3.5wk post-Phase-B, 2-3wk after layout audit). The swap-test validation removed the largest remaining "unknown" risk.

**Files this round**:
- `src/mtr-asi/include/mtr/controlmapper_vtable_probe.h` — added `capture_dev_p2`, getters, and updated `disarm()` to undo swap-test
- `src/mtr-asi/src/coop/controlmapper_vtable_probe.cpp` — added dev_P2 state + capture impl (4KB calloc + SEH memcpy with size fallback) + swap-test arm path
- `src/mtr-asi/src/coop/controlmapper_probe.cpp` — calls `capture_dev_p2()` on first non-null `*(instance+4)` observation
- Build: 689,664 bytes (+1536 over 688,128 prior; +5632 over pre-probe 684,544)

### Phase 2 step (b5) — set_current_dev API + swap-stress test SHIPPED + VALIDATED (2026-05-11 final)

Added boundary-based per-player swap API. Future player-entity-tick hook will call `set_current_dev_for_player(0)` or `(1)` at tick entry/exit; the API atomically rewrites `instance[+4]` to point at dev_P1 or dev_P2 accordingly.

```cpp
void set_current_dev_for_player(int player_idx);
uint64_t swap_count();  // diagnostic
```

Implementation: fast-path no-op if target already matches (avoids unnecessary writes during long player-tick segments), otherwise SEH-guarded single dword write. Mutex serializes against `capture_dev_p2` / `disarm`. Counters track swap operations.

**Swap-stress validation** — new cmdline flag `-mtrasi-cmvt-swap-stress` cycles `instance+4` between dev_P1 and dev_P2 every 100 cm-probe lookups (~67ms cadence). Tests the mechanism under HIGH repeat-rate.

Test results:

| Test | Result | Frames | Time | Swaps performed | Notes |
|---|---|---|---|---|---|
| Baseline (unarmed) | PASS | 1872 | 10.13s | 0 | dev_P1 throughout |
| `-mtrasi-cmvt-swap-test` (one-shot) | PASS | 1871 | 9.86s | 1 | dev_P2 held until shutdown |
| **`-mtrasi-cmvt-swap-stress` (cycling)** | **PASS** | **1872** | **9.34s** | **16** | **P1↔P2 every 67ms, 1.04s armed** |

16 swaps in 1.04s of in-game time with zero engine misbehavior. The mechanism handles per-frame swapping safely. This is the strongest possible validation of the routing-thunk design before implementation.

**What's left for Phase 2 to ship**:
1. **(b1)** — Find a `player_idx` field on the protagonist entity (or add one). ~1-2 days RE.
2. **(b2)** — Hook the player entity tick: call `set_current_dev_for_player(entity->player_idx)` at entry. ~2-3 days (entity-system reads needed).
3. **(b6)** — Live test routing-active with current_player_context fixed = 0 → verify zero behavior change. Now redundant with (b5) stress test? Probably yes — if cycling works, fixed-0 must.
4. **(b7)** — Wire context = 1 inside P2 entity tick → verify P2 input from injected DI works. The integration test.

Phase 2 estimate at this point: **1.0-2.0 weeks** (was 1.5-2.5 pre-(b5)), since the swap-stress test eliminated the post-mechanism-implementation risk.

**Files this round**:
- `src/mtr-asi/include/mtr/controlmapper_vtable_probe.h` — added `set_current_dev_for_player`, `swap_count`
- `src/mtr-asi/src/coop/controlmapper_vtable_probe.cpp` — added swap state (g_swap_count, g_current_player), set_current_dev_for_player impl, stress_test_tick, extended dump_to_log
- `src/mtr-asi/src/coop/controlmapper_probe.cpp` — added stress-test driver tied to cmdline flag, fires every 100 lookups
- Build: 690,688 bytes (+512 over 689,664; +6144 over pre-probe baseline)

### Phase 2 implementation implications (revised after live probe)

1. **Static vtable VA known**: 0x006A639C. We can patch the vtable IN PLACE at boot — no need to intercept the registry lookup at all. **Option B from the prior design section becomes strictly cheaper than Option A.**

2. **Only 13 methods to forward** (not "~10" estimated). Slot count is firm.

3. **Lazy-init window**: between game boot and the first scenario tick, the slot exists with null instance. Our patched vtable would never be called during this window because the engine consumers check the instance pointer (via `sub_5CB160` returning 0) before doing anything. Safe.

4. **Two outers, one slot**: doesn't affect the vtable-patch approach since the patch targets the vtable behind the instance, not the registry. Confirmed via probe that all consumers converge on the same instance regardless of which outer they queried.

5. **Phase 2 estimate refinement**:
   - vtable-patch + 13 thunks: **0.5 wk** (was 0.5-1.0)
   - per-player context wiring: 1.0-1.5 wk (unchanged)
   - construct P2 instance: 1.0 wk (unchanged; still wildcard)
   - live test + iterate: 1.0 wk
   - **Total: ~2.5-3.5 wk** (slight reduction from the ~3-4wk pre-probe estimate)

## Newly characterized engine VAs

| VA | Name | Notes |
|---|---|---|
| `0x5CB310` | `registry_lookup_dword2` | __thiscall(outer, name, unused) — hashtable at outer+8 |
| `0x5CB350` | `registry_lookup_dword1` | __thiscall(outer, name) — same but 1 arg |
| `0x5CB160` | `slot_lazy_init_and_deref` | (slot) → resource ptr when slot.kind==5 |
| `0x5CB420` | `registry_register_typed` | __thiscall(outer, type_id, name, kind, storage, callback) — installs 28-byte slot |
| `0x5D3730` | `hashtable_find` | __thiscall(hashtable, key_bytes, key_len) → &value or 0 |
| `0x5D3E30` | `name_intern` | string → intern id (stored at slot+0x14) |
| `0x4369C0` | `key_hash` | (bytes, len) → hash |
| `0x5832C0` | `mtr_alloc` | (size) → ptr — used to alloc slot record (28 bytes) and kind-specific storage |
| `0x6A6EE0` | `kControlMapperName` | the only live `"ControlMapper"` string literal |
| `0x6A639C` | **`g_ControlMapper_vtable`** | 13 slots; statically-located vtable for ControlMapper class |
| `0x6914F0` | `ControlMapper_dtor_vt0` | vtable[0] — virtual dtor `(this, should_free)` |
| `0x4F92A0` | `nullsub_1` | vtable[1] — true no-op |
| `0x59AAF0` | (single `RET` byte) | vtable[2] — true no-op stub |
| `0x56EEC0` | `ControlMapper_Tick_vt3` | vtable[3] — per-frame ring shift + Update jump |
| `0x56E8D0` | `ControlMapper_GetAnalog_vt4` | vtable[4] — `(this, button_id) → float` analog value |
| `0x56E940` | `ControlMapper_WasJustPressed_vt5` | vtable[5] — edge-detect `prev=0, curr=1` (was mislabeled "IsPressed") |
| `0x56E9C0` | `ControlMapper_WasJustReleased_vt6` | vtable[6] — edge-detect `prev=1, curr=0` |
| `0x56EA40` | `ControlMapper_IsHeldOrAux_vt7` | vtable[7] — held/aux query |
| `0x401000` | `ControlMapper_LoadButtonProfile_vt8` | vtable[8] — 13-case profile loader via `ControlMapper_BindButton` |
| `0x401270` | `ControlMapper_LoadHatProfile_vt9` | vtable[9] — 13-case hat-profile loader via `ControlMapper_BindHat` |
| `0x56EB60` | `ControlMapper_GetMousePos_vt10` | vtable[10] — `(this, &out2)` mouse/cursor pair |
| `0x56EBC0` | `ControlMapper_GetFlagA8_vt11` | vtable[11] — device-alive byte |
| `0x56EE50` | `ControlMapper_GetVec3_vt12` | vtable[12] — `(this, &out3, idx)` vec3 array accessor |
| `0x56E710` | `ControlMapper_BindButton` | helper writer for slot 8 |
| `0x56E8A0` | `ControlMapper_BindHat` | helper writer for slot 9 |
| `0x56E840` | `controlmapper_rumble_pulse` | (slot, intensity\|flags, p3) — NOT a vtable slot |
| `0x56E860` | `controlmapper_rumble_enable` | (bool) — NOT a vtable slot |
| `0x56E880` | `controlmapper_rumble_query` | (out_caps) → bool — NOT a vtable slot |

## Phase 2 step (b1) — Protagonist entity player_idx field audit (2026-05-11 final-late+1)

**Question**: Does the protagonist entity have a built-in `player_idx`-like field that Phase 2's player-entity-tick hook can read to call `set_current_dev_for_player(entity->player_idx)`?

**Answer**: NO. The engine has no per-entity player index. The clean path is a **mod-owned side-table** keyed on instance pointer.

### Audit chain

1. **`protagonist_alloc_and_ctor_3276b` @ 0x5B71C0** (renamed from sub_5B71C0):
   ```c
   void *protagonist_alloc_and_ctor() {
       void *p = mtr_alloc(3276);   // 0xCCC bytes
       return p ? protagonist_derived_ctor(p) : 0;
   }
   ```
   **Protagonist instance size = 3,276 bytes** (firm). No constructor parameter.

2. **`protagonist_derived_ctor` @ 0x5B6F40** (renamed from sub_5B6F40):
   - Calls `Actor_base_ctor(this, 0x718D28)` first.
   - Sets `*this = 0x6CC9A8` (protagonist vtable embedded in class descriptor at 0x6CC980).
   - Touches only ~13 fields, all at offsets 3224..3272 (near end of struct):
     - `this[806] = 0` (+3224)
     - `this[809] = 0x6A6CB8` (+3236) — string ptr
     - `this[810]..this[817] = 0` (+3240..+3268)
     - `this+3248 byte = 0`, `this+3272 byte = 0`
   - **No player_idx field set.** No parameter taken.

3. **`Actor_base_ctor` @ 0x5B3F10** (renamed from sub_5B3F10):
   - Sets `*this = 0x6C8568` (Actor base vtable), then derived ctor overrides.
   - Subobject vtables at `this+56` (= +224) and `this+792` (= +3168).
   - Inits KV-bag at `this+799` (= +3196) via `sub_4B8E40` — this is the documented entity property bag.
   - Calls `Actor_init_state(this)` at end.
   - **No player_idx field set.** Takes one arg (`a2 = 0x718D28`, a descriptor ptr), passes it to `sub_403130`.

4. **`Actor_init_state` @ 0x5B37C0** (renamed from sub_5B37C0):
   - Massive `memset(this+508, 0, 0xA50)` zero-init + many specific field writes.
   - Notable fields: `this+212 = 17`, `this+460 = 1`, `this+464 = 2`, `this+184/+188/+192 = 1.0f`, `this+196 = 100.0f`, `this+212 = 17`.
   - Sets pointers `this+348 = this+88`, `this+352 = this+112` (sub-pointers, intrusive linked list?).
   - **No player_idx field set.** No identifier-like field beyond Actor flags.

5. **`entity_factory_construct` @ 0x5B96F0** (already documented):
   - Looks up class registry by bag's `class` property → calls `entry->vtable[+4](entry, bag)` which is `protagonist_alloc_and_ctor_3276b` for protagonist.
   - **No player_idx parameter** in the construction path. The bag has no `player_idx`-like key.
   - The factory takes spawn-point coords (`a4`, `a5`) but no player identifier.

### Cross-check: slot 5 callers

The 11 distinct slot-5 callers captured at runtime (probed in Phase B) all use the singleton lookup pattern `sub_5CB310("ControlMapper", 0) → sub_5CB160(slot) → call vtable[+0x14]`. None of them pass or read a per-entity player index. They are scattered across various game systems (gameplay button polls), not concentrated in the protagonist tick.

### Implication

Phase 2 step (b1) does NOT require finding an engine field. It requires building a small registration mechanism in the mod:

```cpp
namespace mtr::protagonist_registry {
    // Side-table populated by hooking protagonist_derived_ctor POST-return
    // (and the dtor for cleanup). Bounded to ~2 entries for 2P coop.
    void  register_instance(uint32_t this_ptr);
    void  unregister_instance(uint32_t this_ptr);
    int   player_idx_for(uint32_t this_ptr);  // 0 for first registered, 1 for second, -1 if unknown
}
```

Implementation outline:
- MinHook on `protagonist_derived_ctor @ 0x5B6F40` (POST-return), captures `this` from EAX (cdecl-style return) or stack.
- Maintain a tiny fixed-size array `g_protagonists[2]` ordered by registration time.
- `player_idx_for(this)` linear-searches and returns the index, or -1 if not found.
- Hook the dtor (vtable slot 0 of protagonist vtable @ 0x6CC9A8) to call `unregister_instance` so the slot can be reused if the entity is destroyed mid-game.

This is cheaper than locating a player_idx field that doesn't exist, and avoids any engine memory layout changes. It's also the right factoring: the mod owns the player-identity concept since the engine has no notion of it.

### Phase 2 (b1) status: DONE

The audit proves the field does not exist; the side-table design replaces "find it" with "register on construction". Estimate: ~0.5 day to implement the registry + 0.5 day to wire the ctor hook (was budgeted as 1-2 days; can shorten because the design is now firm).

### Newly characterized engine VAs (this section)

| VA | Name | Notes |
|---|---|---|
| `0x5B71C0` | `protagonist_alloc_and_ctor_3276b` | mtr_alloc(3276) + ctor; class="protagonist". NEVER fires during normal gameplay or save-load (verified by live test). |
| `0x5B6F40` | `protagonist_derived_ctor` | Sets vtable=0x6CC9A8; touches ~13 fields near end of struct; no params |
| `0x5B3F10` | `Actor_base_ctor` | Sets Actor base vtables + KV-bag at +3196; takes descriptor ptr arg |
| `0x5B37C0` | `Actor_init_state` | Bulk memset + field init for Actor state; many defaults |
| `0x6CC980` | `g_protagonist_class_descriptor` | Class header (24 bytes meta) + "Protagonist" name (16 bytes) + vtable starting at +0x28 |
| `0x6CC9A8` | `g_protagonist_vtable` | Protagonist instance vtable (referenced by ctor); slot count TBD |

## Phase 2 step (b2) — Player-entity registry SHIPPED + VALIDATED (2026-05-11 final-late+2)

**Background**: Phase 2 step (b1) found the engine has no built-in `player_idx`. Step (b2) implements a mod-owned side-table that maps `wilbur instance ptr → player_idx`, populated by hooking the wilbur class factory POST-return.

### Class-identity correction (live-test revealed)

Initial assumption (per the Phase 0A audit doc) was that "protagonist is the player class". Live-test on the load-save-1-show-ingame scenario disproved this:
- `class="protagonist"` is NEVER looked up via the class registry during normal gameplay or save-load (verified by the registry-lookup logger in coop_spawn_probe across 611+ class lookups).
- `class="wilbur"` IS the class used for the actual gameplay player.

The "protagonist" class is registered in the class registry (head=0x705454, 17 classes total) but unused by normal gameplay paths. It may be a legacy or abstract base.

### True wilbur class factory: 0x48E030

**The wilbur class registry entry** lives at .rdata `0x6B8614`:

| Offset | Value | Meaning |
|---|---|---|
| +0x00 | `0x0048D5E0` | vtable[0] (some method) |
| +0x04 | `0x0048E030` | **vtable[1] = wilbur allocator + ctor (the function entity_factory_construct dispatches to)** |
| +0x08 | `"wilbur\0"` inline | Class name (10 bytes including padding) |

The class entries in .rdata follow the pattern `{ ptr0; ptr1; inline_name; padding }` — name embedded inline rather than via separate string pointer (consistent with the protagonist class descriptor at 0x6CC980 which embeds "Protagonist" inline).

**`wilbur_class_factory_alloc_ctor` @ 0x48E030** (renamed from sub_48E030):
- Calls `mtr_alloc(3404)` → 3,404-byte (0xD4C) wilbur instance
- Calls `sub_48D780` (a SecuROM thunk to the wilbur derived ctor)
- Sets `*instance = 0x6BC9F0` (wilbur instance vtable)
- Runs massive setup: SimpleGroundController, ActionHandler, GroundFollower, SimpleCollision, **`InventoryList(name="player", 1)`**, HealthTracker, Footfall, AvatarFX, Magnet, OnDemandHelp, etc.
- Sets KV-bag defaults at instance+0xC7C (`v2+799` in decomp) including model_name and feature flags
- Returns initialized wilbur instance

The 3,404-byte wilbur instance is larger than the 3,276-byte protagonist instance — confirms different class. The wilbur instance ALSO uses the entity+3196 KV-bag convention (set at `v2+0xC7C = v2+3196`).

### Other false leads

- `0x48D8A0` is ALSO an `mtr_alloc(3404) + sub_48D780` wrapper, but it's a DIFFERENT code path (probably minigame variant or alternate init). It is NEVER reached by the factory dispatch during normal gameplay. Hooking it produced silent no-ops; my initial registry build targeted this and hit zero registrations.

### Live-test validation

Test scenario `load-save-1-show-ingame` with cmdline `-mtrasi-protag-registry-log`:

```
[18:18:55.838] [protag_registry] installed (wilbur_factory=0x0048E030 log=1)
[18:19:04.716] [protag_registry] REGISTER this=0x0F028FE0 -> player_idx=0
[18:19:05.376] [protag_registry] REGISTER this=0x0EFBFA20 -> player_idx=1
```

- player_idx=0 (this=0x0F028FE0) = gameplay player constructed during save-load
- player_idx=1 (this=0x0EFBFA20) = test_harness's `try_spawn_p2` orphan (torn down ~125ms later)

Test PASSED with frames=1872 elapsed=9.27s — no engine misbehavior. Hook is __thiscall(this, edx_dummy, bag) per the factory dispatch ABI.

### Module: `mtr::protagonist_registry`

**Files added**:
- `src/mtr-asi/include/mtr/protagonist_registry.h` — public API (register/unregister/player_idx_for/observation/install)
- `src/mtr-asi/src/coop/protagonist_registry.cpp` — MinHook on 0x48E030 POST-return; fixed-size array of 2 slots; mutex-guarded
- `src/mtr-asi/CMakeLists.txt` — added source file
- `src/mtr-asi/src/dllmain.cpp` — calls `protagonist_registry::install()` after controlmapper_vtable_probe install

**Public API**:
```cpp
namespace mtr::protagonist_registry {
    void register_instance(uint32_t this_ptr);
    void unregister_instance(uint32_t this_ptr);
    int  player_idx_for(uint32_t this_ptr);        // 0=P1, 1=P2, -1=unknown
    struct Observation { ... };
    Observation observation();
    bool install();
}
```

**Cmdline flag**: `-mtrasi-protag-registry-log` enables per-register/unregister log lines. Default OFF.

### Phase 2 (b2) remaining + (b7) gaps

- No dtor hook yet — the wilbur instance dtor is at `vtable[0]` of the wilbur instance vtable (`*0x6BC9F0`) but the address is currently unknown. The test_harness orphan slot is not freed, so the registry currently fills both slots after one play session. Acceptable for current testing (the live player is in slot 0, where it should be).
- Per-tick `set_current_dev_for_player()` wrapping is NOT yet wired. That's part of (b7) — when a real P2 wilbur exists, we'll hook the wilbur tick method (one of the 67 slots in vtable @ 0x6BC9F0) and call `set_current_dev_for_player(player_idx_for(this))` at entry, `(0)` at exit.

### Build artifact

mtr-asi.asi = **692,736 bytes** (+2,048 over 690,688 prior; +8,192 over pre-probe baseline 684,544).

### Updated VAs

| VA | Name | Notes |
|---|---|---|
| `0x48E030` | `wilbur_class_factory_alloc_ctor` | TRUE wilbur factory dispatched by entity_factory_construct; mtr_alloc(3404) + sub_48D780 ctor thunk + massive component setup |
| `0x48D8A0` | `wilburX_alloc_ctor_3404b_NOT_factory` | NOT the factory — alternate wilbur-class allocator never reached by factory dispatch in normal play |
| `0x6B8614` | `g_wilbur_class_registry_entry` | Class entry: vt[0]=0x48D5E0, vt[1]=0x48E030, inline name "wilbur" at +0x08 |
| `0x6BC9F0` | (NOT the wilbur vtable) | Was claimed as the wilbur instance vtable in earlier notes — **corrected 2026-05-11 (final-late+3)**: 0x6BC9F0 holds engine strings (`"RAY\0"`, `"DB_SNGLSOMBONE"`, etc.), not a vtable. The actual wilbur instance vtable address is below. |
| `0x6B8730` | **`g_wilbur_instance_vtable`** | Real wilbur instance vtable. Set by 0x48E030's `mov dword ptr [esi], 0x6B8730` at 0x48E070 (Hex-Rays renders the constant in decimal as 7046960). **66 slots**, ending at the `"avatars/digDugWilbur.sx"` string at 0x6B8838. Slot 0=dtor (`sub_48EC40`), slot 2=`sub_5B6EF0` (forwards to `this[811]` delegate), slot 4=`sub_6092D0` (stub `return 1`), slot 8=`sub_5B6F30` (returns `"Protagonist"`), slot 17=`sub_5AE740` (property/component lookup-by-name), slot 26=`script_command_dispatch_giant @ 0x5B40C0` (11,342-byte script-command/event handler — NOT a per-frame tick). |

## Phase 2 step (b2-rem) — Per-frame tick discovery + component->player_idx side-table SHIPPED (2026-05-11 final-late+3)

The original Phase 2 (b2-rem) work-item was *"hook the wilbur per-frame tick method and wrap with set_current_dev_for_player(player_idx_for(this)) at entry"*. Execution revealed the strategy needed correction:

### Finding 1 — The wilbur entity has NO per-frame vtable tick

The wilbur instance vtable at 0x6B8730 has 66 slots. None of them is a "per-frame tick" in the sense the (b2-rem) work item assumed:

- **Slot 2** (`sub_5B6EF0` — the most likely per-frame candidate in a typical engine) is a 21-byte forwarder that calls `vtable[+28](arg)` on a sub-object at `this[811]` (= `this+3244`). Not a tick.
- **Slot 4** (`sub_6092D0`) is a 5-byte stub returning 1. Not a tick.
- **Slot 26** (`script_command_dispatch_giant`) is a giant 11,342-byte function but it's an event-driven script-command DISPATCHER (handles named `.sx` script commands sent to the entity), NOT a per-frame tick.
- The rest of the slots are accessors, setters, dtor variants, and string getters (e.g. slot 8 returns `"Protagonist"`).
- `sub_5AD910(wilbur)` at the end of the factory walks `wilbur[833]` (= `wilbur+0xD04`) component chain calling `vtable[+44]` then `vtable[+36]` on each entry — but that's the post-init activation pass, NOT a per-frame tick.

The aggregator at `simulation_tick_aggregator @ 0x67F430` calls `frame_dt_ring_update`, `entity_transform_tick @ 0x4B9F60`, `physics_state_machine_tick`, `trail_subsystem_tick`, `particle_buckets_sweep_a/b @ dword_724DFC/dword_72633C`, `anim_update_all_tracks`. None invokes a per-entity vtable method.

### Finding 2 — Components are ticked by the engine-manager loop @ sub_5AD4D0

The actual per-frame component-tick driver is **`sub_5AD4D0`** (the entity-manager top-of-frame sweep), called from **`engine_pump_alt @ 0x682010`** at offset 0x68201E (BEFORE `simulation_tick_aggregator`).

Body (two sequential loops over the same global list head):

```c
for ( i = *(_DWORD **)(this + 312); i; i = (_DWORD *)i[8] )
    (*(void (__thiscall **)(_DWORD *))(*i + 8))(i);     // slot 2 (byte offset +8)
...
for ( j = *(_DWORD **)(this + 312); j; j = (_DWORD *)j[8] )
    (*(void (__thiscall **)(_DWORD *))(*j + 16))(j);    // slot 4 (byte offset +16)
```

Layout:
- `g_entity_manager+312` = head of global tickable list (this is NOT the same list as a wilbur's per-entity `wilbur+0xD04` chain)
- Each chain node: `node[0]` = payload (component instance with vtable), `node[8]` (= node[+0x20] = 32 bytes in) = next-node pointer
- Loop dispatch: `payload->vtable[slot 2](payload)` then `payload->vtable[slot 4](payload)`

So when the engine asks "what is button X right now?" via the singleton ControlMapper, the call site lives inside a **component's vtable slot 2 or slot 4 implementation** — a class-specific method.

The contradictory observation about `sub_48A130` (the 4066-byte ControlMapper-reading function, hosted at vtable 0x6B7C00 slot 11, which had `0.003`-dt math) resolves cleanly: vtable 0x6B7C00's slot 11 IS the `is_enabled` predicate per `sub_5AD910`'s convention, but `sub_48A130` is invoked DIRECTLY (vtable-indirected from another caller that uses `vtable[+44]` as the per-frame tick on this specific class — there is no single global "vtable slot is always tick" convention; per-class hosts inherit and override at different slots).

### Finding 3 — Component->wilbur back-pointer is per-class

Initial hypothesis was `*(component + 4) = wilbur` universally. Spot-check:
- vtable 0x6B7C00 family (the `sub_48A130` tick class): `*(this+4)` IS the wilbur. Confirmed by `0x48A1B7  mov eax, [esi+4] ; lea ecx, [eax+0xCCC]` (offset 0xCCC = 3276 bytes = `wilbur[+819*4]`, the KV-bag thunk slot).
- vtable 0x6B7BB0 sibling: uses `*(this+0x14)` for an internal state struct, NOT `+4`. Different layout.

There is no shared base ctor that writes `+4 = wilbur` across all component classes. The class-agnostic mapping must go through the wilbur-side chain, not a fixed component-side offset.

### Strategy — class-agnostic side-table built at wilbur-factory POST

Walk the per-entity component chain at `wilbur+0xD04` (= `wilbur[833]`) at factory POST-return. Each chain node: `{node[0]=component_ptr, node[+0x04]=prev_node, node[+0x08]=next_node}` (12-byte node). For each `node[0]`, register `component_ptr -> player_idx_of(wilbur)` in a side-table. Per-frame hot paths look up by component_ptr in O(1).

This is class-agnostic (works for every component class regardless of internal layout) and avoids the "find the back-pointer offset for class X" rabbit hole.

### Module: `mtr::coop_component_registry`

**Files added**:
- `src/mtr-asi/include/mtr/coop_component_registry.h` — public API
- `src/mtr-asi/src/coop/coop_component_registry.cpp` — SEH-guarded chain walker; `std::unordered_map<uint32_t, int>` storage; mutex-guarded
- `src/mtr-asi/CMakeLists.txt` — added source file
- `src/mtr-asi/src/coop/protagonist_registry.cpp` — calls `coop_component_registry::register_wilbur_components()` after `register_instance()` inside the wilbur-factory POST hook
- `src/mtr-asi/src/dllmain.cpp` — calls `coop_component_registry::install()` after `protagonist_registry::install()`

**Public API**:
```cpp
namespace mtr::coop_component_registry {
    void register_wilbur_components(uint32_t wilbur_ptr, int player_idx);
    int  player_idx_for_component(uint32_t component_ptr);   // 0/1 or -1
    struct Observation { int total_wilburs_walked; int total_components;
                         int max_chain_length_seen; uint64_t total_lookups;
                         uint64_t total_lookups_unknown; };
    Observation observation();
    bool install();
}
```

**Cmdline flag**: `-mtrasi-coop-comp-registry-log` enables per-walk log lines. Default OFF.

### Live-test validation (2026-05-11)

Scenario `load-save-1-show-ingame` with `-mtrasi-protag-registry-log -mtrasi-coop-comp-registry-log`:

```
[18:52:12.178] [protag_registry] installed (wilbur_factory=0x0048E030 log=1)
[18:52:12.179] [coop_comp_registry] installed (log=1)
[18:52:22.269] [protag_registry] REGISTER this=0x0EECBF60 -> player_idx=0
[18:52:22.269] [coop_comp_registry] wilbur=0x0EECBF60 idx=0 walked=40 registered=40
[18:52:22.943] [protag_registry] REGISTER this=0x0EE629B0 -> player_idx=1
[18:52:22.943] [coop_comp_registry] wilbur=0x0EE629B0 idx=1 walked=40 registered=40
```

- 40 components walked + registered per wilbur (gameplay P1 at idx=0; test_harness `try_spawn_p2` orphan at idx=1).
- 80 component entries in the side-table after one play session.
- Test PASSED frames=1934 elapsed=9.81s — chain walk introduces no engine misbehavior.
- SEH guard never triggered (chain is well-formed at factory POST-return).

### Phase 2 next gates (post-b2-rem)

The component->player_idx infrastructure is now ready. Next decisions:

1. **(b2-rem-2) Routing thunk strategy**. Three viable approaches to actually consume the side-table at per-frame tick boundaries:
   - **a. Per-instance vtable cloning**: at registration time, allocate a per-instance vtable copy with slot 2 / slot 4 replaced by interposers that call `set_current_dev_for_player(player_idx_for_component(this))` PRE and `(0)` POST. Cleanest, but variable per-class vtable size (need to determine slot count per class).
   - **b. sub_5AD4D0 re-implementation**: replace the function entirely with a mod-side loop walker that brackets each slot 2 / slot 4 dispatch with the context swap. Higher coupling to engine internals.
   - **c. ControlMapper-level routing via caller stack inspection**: walk caller stack from inside CM vtable thunks to identify the owning component → owning wilbur. Brittle.
   
   Recommended: **(a)** — clean per-instance interposition. Done at component register time (hook `sub_5D3B80`).

2. **(b7) P2 spawn integration**: once routing thunks ship, spawn a P2 wilbur via `entity_factory_construct(class="wilbur", ...)`, inject P2 DI input via `dinput_hook::inject_kb_keypress`, verify P2 reads P2's dev.

### Build artifact

mtr-asi.asi = **694,784 bytes** (+2,048 over 692,736 prior; +10,240 over pre-probe baseline 684,544).

### Updated VAs

| VA | Name | Notes |
|---|---|---|
| `0x5AD4D0` | `entity_manager_tick_components` | Per-frame component-tick driver; two loops over `g_entity_manager+312` global chain calling vt[slot 2] then vt[slot 4] on each payload |
| `0x682010` | `engine_pump_alt` | Frame pump entry; calls `entity_manager_tick_components` at +0x0E, then `simulation_tick_aggregator` |
| `0x5D3B80` | `component_registry_insert` | Global tickable registration entry point (sibling of `sub_5D3C80`); future hook target for per-instance vtable cloning |
| `0x6B8730` | `g_wilbur_instance_vtable` | (entry above corrected — was 0x6BC9F0 in prior memory) |

## Phase 2 step (b2-rem-2) — Class-wide in-place vtable routing thunks SHIPPED (2026-05-11 final-late+4)

The component->player_idx side-table (b2-rem) needed a consumer at per-frame tick boundaries. Two design attempts:

### Attempt 1: per-instance vtable cloning — CRASHED

Initial design: for each wilbur-owned component, allocate a clone of the engine's static vtable (256 bytes — 64 slots over-copied), patch clone slots 2 and 4 to point at our routing thunks, and rewrite `*component = &clone->slots[0]`. Stored `orig_vt` in a header dword at `clone-4` so thunks could find the original.

Live test crashed with AV at `eip=0x006390C9` (`sub_6390B0` — case-insensitive string compare, reading null arg). Diagnosis via a "dry-run" build (clone everything but skip the `*component` rewrite) passed cleanly. **Conclusion**: the engine does pointer-identity checks on vtable addresses somewhere; rewriting `*component` to a heap clone breaks them. The clone bytes were correct, but the identity changed.

### Attempt 2: class-wide in-place vtable patching — SHIPPED

For each unique component vtable encountered via the wilbur+0xD04 walk:
1. `VirtualProtect(vt_addr, 32, PAGE_READWRITE, &old)` — lift the .rdata read-only bit on a 32-byte window covering slots 0..7.
2. Save `vt[+8]` and `vt[+16]` (slots 2 and 4) to a `std::map<uint32_t, OrigSlots>` keyed by vtable address.
3. SEH-guarded write of `vt[+8] = &thunk_slot2`, `vt[+16] = &thunk_slot4`.
4. `VirtualProtect(...)` restore.

Engine identity checks (`*component == orig_vt`) still pass because we didn't rewrite `*component`. The thunks read `*this` to recover the (unchanged) vtable address, then look up the original slot via the map.

**Trade-off**: class-wide vs per-instance. Class-wide means every instance of the patched class globally — not just our wilbur's — goes through the thunk. For non-wilbur instances `player_idx_for_component(this)` returns -1; the thunk skips `set_current_dev_for_player` and just forwards. Cost ≈ 30-50 cycles per non-routed call.

### Critical thunk bug — return-value preservation across cleanup helper

The naked thunks fire in sequence:
1. preserve callee-saved (`push ebx; push esi; push edi`)
2. `idx = thunk_helper_get_idx(this)`
3. `if (idx >= 0) thunk_helper_set_dev(idx)` — pre-call swap
4. `orig_slot = thunk_helper_get_orig_s2(*this)` — map lookup
5. `call orig_slot(this)` — dispatch
6. `if (idx >= 0) thunk_helper_set_dev(0)` — post-call restore
7. restore + ret

**Bug discovered live**: the post-call helper at step 6 is `__cdecl`, which clobbers caller-saved `eax`/`ecx`/`edx`. But many slot 2 implementations are **class-name getters** (e.g., `0x004F8CF0 = mov eax, 0x6A997B; ret` returns a class name string ptr in EAX). After step 5 EAX holds the orig return value; step 6 clobbers it; the engine receives a garbage return → downstream null-deref in `sub_6390B0`.

**Fix**: save and restore EAX/EDX across the post-call cleanup:
```asm
after_orig:
    push    eax      ; preserve orig's return value (eax 32-bit; edx for 64-bit returns)
    push    edx
    cmp     esi, 0
    jl      skip_restore
    push    0
    call    thunk_helper_set_dev
    add     esp, 4
skip_restore:
    pop     edx      ; restore in reverse
    pop     eax
```

After this fix the armed run passed cleanly: 1874 frames, 8.78s, 40 unique vtables patched class-wide.

### Module additions

`src/mtr-asi/src/coop/coop_component_registry.cpp` now contains:
- `std::map<uint32_t, OrigSlots> g_orig_slots_by_vt` — keyed by vtable address; values are `{orig_s2, orig_s4}` fn pointers
- `extern "C"` map-lookup helpers `thunk_helper_get_orig_s2/_s4` for the thunks
- Naked thunks `thunk_slot2` / `thunk_slot4` (`__declspec(naked)` with inline asm)
- `install_routing_for_component_locked(component_ptr)` — class-wide in-place patcher; idempotent (skip if vtable already in map)

New cmdline flags (default OFF):
- `-mtrasi-coop-router-arm` — install thunks
- `-mtrasi-coop-router-dry` — alloc/probe but skip the actual write (diagnosis aid)

`Observation` struct extended with `routing_armed`, `total_thunks_installed`, `total_thunks_skipped`, `thunk_fires_slot2`, `thunk_fires_slot4` counters.

### Live-test matrix (all PASS)

| Flags | Result | Frames | Time | Notes |
|---|---|---|---|---|
| (none) | PASS | 1870 | 8.81s | baseline |
| `-arm` (per-instance clone) | **CRASH** | — | — | AV at sub_6390B0 (engine vtable identity check) |
| `-arm -dry` (clone but no rewrite) | PASS | 1872 | 9.34s | confirmed *component rewrite is the issue |
| `-arm` (class-wide in-place, no EAX save) | **CRASH** | — | — | EAX clobbered across post-call helper |
| `-arm` (class-wide in-place, EAX/EDX preserved) | **PASS** | **1874** | **8.78s** | **shipped** |

40 unique vtables patched class-wide (P1's wilbur). P2's wilbur (the test_harness orphan) walked 40 components but added 0 new patches — same component classes, idempotent skip.

### Build artifact

mtr-asi.asi = **697,856 bytes** (+3,072 over 694,784 prior; +13,312 over pre-probe baseline 684,544).

### Phase 2 next gates (post-b2-rem-2)

The mechanism is now end-to-end:
1. **At wilbur factory POST**: walk component chain → side-table population → class-wide vtable patching (40 vtables).
2. **At per-frame dispatch (slot 2 / slot 4)**: thunk fires → side-table lookup → set_dev(idx) PRE / set_dev(0) POST.

What's left for full Phase 2:
- ~~**(b6)** Verify with context fixed at 0 (no P2 input source) that the routed build behaves identically to the unrouted build over a longer scenario. Already validated for 9s; should extend.~~ **SHIPPED — see (b6) section below.**
- **(b7)** Spawn a real P2 wilbur via `entity_factory_construct(class="wilbur", ...)` with the correct bag, inject P2 DI input via `dinput_hook::inject_kb_keypress`, verify the P2 wilbur reads from `dev_P2` (currently a memcpy of dev_P1 — same input — so behavior is identical to P1; need to differentiate dev_P2's button-ring to actually drive P2 separately).
- **(opt)** Add a fires-counter dump on test pass (via `test_harness` calling `coop_component_registry::dump_to_log`) so the per-slot tick rate is visible in autonomous test artifacts.

## Phase 2 step (b6) — Extended fixed-context-0 soak validation PASS (2026-05-11 final-late+5)

### Goal

Validate that the routed build (class-wide vtable patching of 40 wilbur component classes, slot 2 + slot 4 wrapped in `set_current_dev_for_player()` PRE/POST) behaves identically to the unrouted build over ~60s of live gameplay — vs. the 9s window validated by `load-save-1-show-ingame`. With only P1 registered, `player_idx_for_component(this)` returns 0 for all P1 components, so the PRE call is `set_dev(0)` and POST is `set_dev(0)` — a behavioral no-op against the engine's default state. If the routing introduces any per-tick regression (register clobber, vtable-identity drift, stale lookup, lock contention), 60s × ~720 fires/sec = ~43k thunk invocations gives it room to surface.

### Implementation

New scenario `coop-router-soak` in `src/mtr-asi/src/test_harness.cpp`:
1. Reuses `load-save-1-show-ingame`'s phases A-E (boot dismiss → menu drive → slot 1 confirm → CONTINUE GAME → gameplay detect).
2. Once `is_in_gameplay()` returns true, enters new phase `kSoakHoldInGameplay`: hold for `kSoakDurationFrames = 14400` (~60s @ 240Hz), emitting a heartbeat every `kSoakHeartbeatFrames = 1200` (~5s) that samples `coop_component_registry::observation()`.
3. Fail criterion: `is_in_gameplay()` returning false mid-soak (player entity lost) → `Result::Fail`.
4. Pass criterion: reach end of hold window with stable player entity → `Result::Pass` with `detail` containing the final observation snapshot for harness JSON capture.

### A/B results (build 701,952 bytes)

| Run | Cmdline flag | result | frames | elapsed_ms | fps | armed | thunks_installed | fires_s2 | fires_s4 | lookups | unknowns | walks | components |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| baseline | (none) | **pass** | 16153 | 68953 | 234.3 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 40 |
| armed | `-mtrasi-coop-router-arm` | **pass** | 16153 | 69359 | 232.9 | 1 | 40 | 43434 | 29 | 43463 | 31 | 1 | 40 |

### Key observations

1. **Identical frame count** (16153 each). Sim ran for the same number of frames in both runs — no scheduling change.
2. **Δ elapsed = +406ms (+0.6%)** — within run-to-run noise for a 70s test on Windows.
3. **Class-wide patching activated 40 distinct vtables** for the P1 wilbur. Same `components=40` count in both runs → registry walk path is unaffected by the arm flag (correct: arming only adds the in-place vtable patch step at the same time we add to the side-table).
4. **Slot-2 thunk fire rate is steady at ~720/sec** across all 12 heartbeats (5s windows ranged 3600–3608 fires). Slot-4 fires stabilize at 29 after T0 (most component classes have slot 4 = `sub_6092D0` stub `return 1`, only initial-setup-style components touch it).
5. **`unknowns=31` is stable** from HB1 onward. Read: 31 components share class with registered P1 components (so their vtable was patched class-wide) but their `component_ptr` was never added to the side-table — likely late-spawned scene/streaming-system components that share vtable with the wilbur's set. For these, the thunk's `player_idx_for_component()` returns -1, the `cmp esi, 0; jl skip_set2` path runs, the `set_dev()` call is skipped, and the orig is invoked unchanged. This is the correct fallback path.
6. **Player entity identity stable**: `player=0EE25FE0` unchanged from gameplay-detect through SOAK COMPLETE.
7. **`min_tcount=0`** in both runs is a pre-existing edge case: `is_in_gameplay()` returns true on player-entity-existence, but `count_transform_list` returns 0 momentarily during scene transitions. Sparse-level (Robinson House intro) has `tcount=1` once settled, but starts at 0 right at gameplay detect. Not router-induced (shows in baseline too).

### Heartbeat trace (armed run, abridged)

```
T0          frame=1693  walks=1 components=40 fires_s2=46    fires_s4=5  lookups=51    unknowns=0
HB held=1200            fires_s2=3654   fires_s4=29  lookups=3683   unknowns=31  (+3608 fires_s2 in 5s = 721/s)
HB held=2400            fires_s2=7254   fires_s4=29  lookups=7283   unknowns=31
HB held=3600            fires_s2=10854  fires_s4=29  lookups=10883  unknowns=31
HB held=4800            fires_s2=14454  fires_s4=29  lookups=14483  unknowns=31
HB held=6000            fires_s2=18054  fires_s4=29  lookups=18083  unknowns=31
HB held=7200            fires_s2=21654  fires_s4=29  lookups=21683  unknowns=31
HB held=8400            fires_s2=25254  fires_s4=29  lookups=25283  unknowns=31
HB held=9600            fires_s2=28854  fires_s4=29  lookups=28883  unknowns=31
HB held=10800           fires_s2=32454  fires_s4=29  lookups=32483  unknowns=31
HB held=12000           fires_s2=36054  fires_s4=29  lookups=36083  unknowns=31
HB held=13200           fires_s2=39654  fires_s4=29  lookups=39683  unknowns=31
HB held=14400           fires_s2=43254  fires_s4=29  lookups=43283  unknowns=31
SOAK COMPLETE after 14400 frames (tcount final=1 min=0 player=0EE25FE0)
```

Per-heartbeat slot-2 fire delta is essentially constant (3600±8 across 5s windows) → steady-state routing with no leak / runaway growth.

### Conclusion

(b6) gate **CLEARED**. The class-wide in-place vtable routing built in (b2-rem-2) operates at production stability levels (~720 thunk invocations per second sustained for 60s with zero player-entity loss, zero crash, zero detectable performance regression).

Cleared: routing infrastructure is ready for (b7) — real P2 spawn + DI inject + dev_P2 differentiation.

### Build artifact

mtr-asi.asi = **701,952 bytes** (+4,096 over 697,856 prior). Delta accounts for the `coop-router-soak` scenario function.

### Test run artifacts

- baseline: `tools/test-runs/20260511-140003-coop-router-soak/`
- armed:    `tools/test-runs/20260511-140132-coop-router-soak/`

## Phase 2 step (b7.1) — Keep-alive crash class identified: `Rider` (2026-05-11 final-late+6)

### Goal

(b7) needs a P2 wilbur that survives past the next sim tick. Per
`coop-phase-0b-breadcrumb-trail-2026-05-10.md` Phase 0C-step-2j, the orphan
spawned by `coop_spawn_probe::try_spawn_p2()` AVs ~150ms after factory return,
inside `sub_5CB160 + 3` with `ecx (this) = NULL`. Step-2j characterized the
fault as a 6-deep call chain rooted at `entity_manager_tick_components
(sub_5AD4D0)`. (b7.1) closes the remaining gap: identify the component class on
the orphan's chain whose `vtable[13]` dispatch produces the NULL.

### RE walkthrough

`sub_5AD9B0(scene+0x20-derived this)` — body:
```c
sub_5B39C0();
result = sub_5AFAD0(this);                   // = this[+0xD4] & 1  (precondition)
if ((BYTE)result) {
    result = (_DWORD *)*(this + 833);        // this+0xD04 = component chain head
    if (result) do {
        v3 = (_DWORD *)result[2];                                                          // next node
        (*(void (__thiscall **)(_DWORD))(*(_DWORD *)*result + 52))(*result);              // (*result).vtable[13](*result)
        result = v3;
    } while (v3);
}
```

So `this+833` is the same `wilbur+0xD04` chain that `coop_component_registry`
walks; **`sub_5AD9B0` IS the per-tick driver that fires vtable[13] on each
component** (sibling to the vtable[2]/vtable[4] driver in `sub_5AD4D0` already
characterized).

`sub_5EBA80` (vtable[13] target for the crashing class):
```c
result = (_DWORD *)sub_55D8F0(*(this + 1));   // arg = component+4
if (result) {
    result = (*result).vtable[6](result);     // first inner dispatch
    if ((BYTE)result) { ...registry/hash lookup; reads dword_7427C0, dword_6CBD8C... }
}
```

`sub_55D8F0`:
```c
v1 = dword_7298A0;                                              // global hint cache
if (!dword_7298A0) { v1 = (char *)&dword_72988C + 1;            // lazy-init sentinel
                     dword_72988C = v1; dword_7298A0 = v1; }
v2 = (*(int (__thiscall **)(int, char *))(*(_DWORD *)a1 + 68))(a1, v1);   // a1->vtable[17](a1, v1)
return v2 ? v2 - 4 : 0;
```

`sub_55D8F0` invokes `a1->vtable[17]` where `a1 = component+4`. That vtable[17]
target on the crashing path is a SecuROM stolen-byte stub at `~0x5454EA`
(IDA static-analysis shows raw bytes; the real target only exists post-rr01
unpack at runtime). The real target ends up calling `sub_5CB160(NULL)` —
which faults at `mov al, [esi+4]` with `esi = ecx = NULL`.

### Class identification

`sub_5EBA80` has zero **code** xrefs but ONE **byte-level** occurrence of its
address `0x005EBA80` in the binary: at `0x6D122C` (`find_bytes '80 BA 5E 00'
→ 0x6d122c`). Slot index 13 ⇒ vtable base = `0x6D122C - 13*4 = 0x6D11F8`.

The vtable at `0x6D11F8` has exactly ONE code xref (`0x5EBA66`, inside
`sub_5EBA20`). `sub_5EBA20` decompiles to:
```c
const char *sub_5EBA20() { return "Rider"; }
```

So **vtable @ 0x6D11F8 is the `Rider` class's vtable**, and `sub_5EBA20` is its
class-name accessor (one of the standard slots — typically vtable[1] or wherever
the engine puts `getClassName()`). The full crashing class is confirmed
as `Rider`.

### Significance

`Rider` is the wilbur's vehicle/ride controller — used in mini-games (DigDug,
ChargeBall, slug-throw etc.) where the wilbur "rides" an entity. Its tick
queries a per-wilbur registry slot via `sub_55D8F0 → vtable[17]` (SecuROM
thunk → real lookup); that slot is populated for the engine's primary wilbur
during normal init paths (level load wires the Rider's host registry) but
NOT for an orphan factory-spawn that bypassed level-load init.

### Vtable layout (Rider class, vtable @ 0x6D11F8, first 16 slots)

| Slot | VA | Role (inferred) |
|---|---|---|
| 0 | 0x5EDAE0 | dtor (decompile failed — likely stolen-byte / unanalyzed) |
| 1 | 0x5EBA20 | `getClassName()` → "Rider" |
| 2 | 0x4F8CF0 | per-frame tick (decompile failed — region partly undefined) |
| 3 | 0x59AAF0 | (shared pass-through helper) |
| 4 | 0x438680 | (component-tick slot-4 stub) |
| 5 | 0x438690 | |
| 6 | 0x422D00 | |
| 7 | 0x452050 | |
| 8 | 0x432610 | |
| 9 | 0x4A6960 | |
| 10 | 0x422D20 | |
| 11 | 0x422D30 | |
| 12 | 0x59AAF0 | (shared with slot 3) |
| **13** | **0x5EBA80** | **per-tick lookup ⇒ crashing dispatch** |
| 14 | 0x5EBB40 | |
| 15 | 0x5EBC40 | |

Slots 3 / 12 / 17 / 18 / 19 all = `0x59AAF0` — a generic pass-through helper used
across many engine vtables.

### Implications for (b7) plan

Three remediation paths, ordered by cost vs. coverage:

1. **Path A (quick derisk, days)** — At wilbur-factory POST-return for any
   instance whose `player_idx == 1` (or `>0`), walk the chain at `+0xD04` and
   unlink any node whose `vtable == 0x6D11F8` (Rider). The orphan never ticks a
   Rider → never reaches sub_55D8F0 → no crash. Acceptable IF mini-game coop
   isn't a Phase 2 requirement (it isn't — coop v2 plan is 2P platforming,
   mini-games stay P1).

2. **Path B (proper fix, weeks)** — VEH-capture the runtime EIP of vtable[17]
   (SecuROM thunk has resolved to its real target by post-unpack), decompile
   the real target, identify which lookup returns NULL on orphan, seed that
   lookup. Cleanest long-term answer but multi-week.

3. **Path C (defensive engine patch, days)** — Hook `sub_5CB160` PRE; if
   `this == NULL`, return 0 early. Hides the crash but leaves the Rider tick
   semantically broken. Acceptable as a fallback if Path A and B both fail.

### Build artifact

No code change yet — RE-only step.

## Phase 2 step (b7.1-rev) — ROOT CAUSE: ViewDriver, not Rider (2026-05-11 final-late+7)

### Correction

Earlier (b7.1) section misidentified Rider as the crashing class. Further RE
disambiguated: **the crashing class is `ViewDriver`** (vtable `0x6C4D40`,
class-name accessor at slot[1] = `0x544E00` returns string `"ViewDriver"` at
`0x6ADA4C`). Rider's vtable[13] (`sub_5EBA80`) is also on the chain and also
calls `sub_55D8F0` — but `sub_55D8F0(rider+4)` resolves to wilbur's vtable[17]
(`sub_5AE740`, fully analyzable) which walks the component chain looking for
the first component whose vtable[6]+vtable[26] both return truthy.
`sub_5AE740` itself doesn't crash on the orphan; it eventually dispatches to
ViewDriver's vtable[13] = `0x5454B0`, which IS the crash site.

### How ViewDriver was found

1. Wilbur's vtable @ `0x6B8730` slot[17] (offset 68) = `0x5AE740` (fully
   analyzable in IDA). It walks the component chain at `wilbur+0xD04` via
   helpers `sub_5AE610` / `sub_5AE650` looking for a component whose
   vtable[6]+vtable[26] both return truthy.
2. The unanalyzed bytes near `0x5454EA` (per breadcrumb step-2j) must
   therefore be a component vtable slot, NOT wilbur's slot.
3. `find_bytes "?? 54 54 00"` returns five matches; three are in .rdata
   vtables (`0x6C4CF0`, `0x6C4D40`, `0x6C4D74`).
4. Reading the vtable at `0x6C4D40`: slot[1] (class-name) =
   `mov eax, 0x6ADA4C; ret`; the string at `0x6ADA4C` is `"ViewDriver"`.
5. Slot[13] of ViewDriver = `0x5454B0` is the unanalyzed function. IDA
   `define_func` fails (likely an alignment quirk + IDA's auto-analysis
   bailout near rr01 boundary); however the bytes decode cleanly:

```asm
0x5454B0  sub     esp, 0x18
0x5454B3  push    esi
0x5454B4  mov     esi, ecx              ; esi = this (ViewDriver)
0x5454B6  mov     eax, [esi]            ; eax = ViewDriver->vtable
0x5454B8  call    [eax+0x18]            ; vtable[6](this) — "needs init" check
0x5454BB  test    al, al
0x5454BD  jz      0x54572F              ; skip if not active
0x5454C3  mov     ecx, [0x6FFCBC]       ; dt (global frame-dt)
0x5454C9  mov     eax, [esi+4]          ; eax = this->owner = wilbur
0x5454CC  push    edi
0x5454CD  push    0                     ; sub_5CB310 arg2 (unused)
0x5454CF  mov     [esp+0x14], ecx       ; save dt
0x5454D3  push    0x6A6EE0              ; "ControlMapper"
0x5454D8  lea     ecx, [eax+0xCCC]      ; ecx = wilbur->per_instance_registry
0x5454DE  call    sub_5CB310            ; lookup "ControlMapper" in registry
0x5454E3  mov     ecx, eax              ; ecx = slot ptr (NULL on orphan)
0x5454E5  call    sub_5CB160            ; ← AVs here when ecx=NULL
0x5454EA  mov     edi, eax              ; ← address breadcrumb step-2j reported
0x5454EC  ...                           ; resumes assuming non-NULL
```

### The actual crash mechanism

```text
entity_manager_tick_components (sub_5AD4D0)
  -> scene+0x20.vtable[1]
    -> (intermediate)
      -> sub_5AD9B0(wilbur)                          // wilbur vtable[21]
        -> walks wilbur+0xD04 chain
          -> dispatches vtable[13] on each component
            -> for ViewDriver: 0x5454B0
              -> sub_5CB310(wilbur+0xCCC, "ControlMapper", 0)
                -> returns NULL on orphan (registry has no entry)
              -> sub_5CB160(NULL)
                -> mov al, [NULL+4]   ← AV @ 0x5CB163
```

(The deeper sub_55D8F0 chain Rider takes is a separate code path that also
runs but doesn't crash — sub_5AE740 either finds no acceptor and falls to
sub_5B6F10, or finds an acceptor whose vtable[26] is a stub. We didn't need
it.)

### Architecture insight (significant)

**Each wilbur has its own ControlMapper registry slot at `+0xCCC`.** The
earlier interpretation of the input-separation point as "the
`ControlMapper` singleton" was based on (a) only ONE string literal
`"ControlMapper"` having active xrefs, and (b) `sub_5CB310`'s `a2`
parameter being unused. Both observations remain TRUE, but the inference
was incomplete:

| Earlier view | Corrected view |
|---|---|
| One global ControlMapper instance | One ControlMapper **slot per wilbur** at `+0xCCC` |
| All 60 callers funnel to the same instance | All 60 callers funnel through `sub_5CB310(entity+0xCCC, "ControlMapper", 0)`, which is per-entity-registry |
| Per-player separation needs vtable thunks | Per-player separation falls out naturally if each wilbur has its own registry slot pointing at a per-wilbur dev |

So the SHARED-instance observation came from there being ONE wilbur in
normal gameplay — every "ControlMapper" query naturally hits the same
slot. With TWO wilburs (engine + orphan), each has its OWN registry; the
engine wilbur's is populated by level-load init, the orphan's is empty.

### Implications for Phase 2

- **(b2-rem-2) class-wide vtable thunks remain valid** — they route the
  *shared engine-side ControlMapper's dev pointer*, which is what the
  engine wilbur's components see today. b6 validated stability.
- **But the more elegant long-term architecture** is per-wilbur
  ControlMapper instances in each wilbur's `+0xCCC` registry — no
  routing thunks required, each wilbur queries its own ControlMapper
  directly. This is multi-week work (b7.2/b7.3 in the new framing).
- **Immediate orphan keep-alive fix**: populate orphan's `+0xCCC`
  registry with a "ControlMapper" entry pointing at the same ControlMapper
  the engine uses. Crash gone; b2-rem-2 thunks route dev_P2 on
  component-tick boundaries for the orphan's components.

### Remaining unknowns (likely follow-on crashes after ControlMapper fix)

The orphan's `+0xCCC` registry may be missing OTHER standard wilbur keys
that other components query during tick. Known queries from RE'd
neighbours:
- `"groundPosition"`, `"groundNormal"`, `"onGround"` (sub_5EADE0 —
  KinematicController path)
- `"KinematicController"` (sub_5EADE0)
- `"ControlMapper"` ✓ (this finding)
- Others surface as we run further with each fix.

So orphan keep-alive may be an iterative process: fix ControlMapper, run
the harness, see what crashes next, fix that, repeat. The proper
long-term fix is whatever populates the engine wilbur's registry during
level-load init — find it, replicate it for the orphan. This is the
Path B work the user authorized (Rule №1).

### Build artifact

Still no code change — RE-only step, finding documented for next-step
planning.

## Phase 2 step (b7.1-next) — Static hunt for the "ControlMapper" writer (2026-05-11 final-late+8)

### Goal

Now that we know the orphan crashes because `wilbur+0xCCC` has no
"ControlMapper" entry, find what writes it on the engine wilbur during normal
init — so we can replicate it for the orphan factory path.

### Method

1. The registry insert function is `sub_5CB420` (signature:
   `__thiscall(registry, value, key_str, type, flag, flag)`). Lookup
   confirmed by inspecting its body: allocates 28-byte slot, hashes the
   name via `sub_5D3E30`, calls `sub_5EDF50(name, len, slot, hash)` to
   index in the hashtable at `registry+8`.
2. `sub_5CB420` has 46 code xrefs. The wilbur-class ctor
   (`wilbur_class_factory_alloc_ctor` @ `0x48E030`) calls it 10 times with
   `(v2+819=v2+0xCCC, v2, key, type, 0, 0)` — exactly the per-wilbur
   registry pattern. But the 10 keys it inserts are
   `"health"`, `"focusOffset"`, `"AvatarFriendly"`, plus 7 unk_XXXXXX
   addresses. **None is "ControlMapper"**.
3. Searched for static occurrences of `"ControlMapper"` (string at
   `0x6A6EE0`):
   - `find type=immediate target=0x6A6EE0` → **60 hits, all in code,
     all are `push 6A6EE0h` immediate**.
   - `find type=data_ref target=0x6A6EE0` → 3 hits, all in
     rumble code (`sub_5B8D80`, `sub_5F2440`, `sub_5F2F50`).
   - `find_bytes "E0 6E 6A 00"` (the address as little-endian dword in
     .data/.rdata) → 60 hits, same as immediate set.
   So `0x6A6EE0` is **never stored in a static data structure** —
   only ever pushed as a literal in code.
4. Sampled the 60 immediate-push sites (`0x40A66E`, `0x412621`,
   `0x489DB9`, `0x4985B0`, …): every one decodes to a **reader**
   pattern — `push 0; push 0x6A6EE0; lea ecx,[X+0xCCC]; call
   sub_5CB310; call sub_5CB160`. No `call sub_5CB420` follows any
   `push 0x6A6EE0`.
5. Therefore the writer is NOT a hardcoded `push 0x6A6EE0` site.
6. `sub_5AD890` ("find or construct component") is the per-class
   component-attach path; it inserts into `wilbur+0xD04`'s CHAIN
   (different structure from the registry at `+0xCCC`). The wilbur ctor
   calls it dozens of times with class names — but none is
   "ControlMapper" (confirmed by reading the strings at each
   `unk_XXXXXX` slot used in calls).
7. The wilbur's base ctor is dispatched via `sub_48D780`, which
   decompiles as a **SecuROM thunk forwarder**:
   ```c
   int __thiscall sub_48D780(void *this, int a2, int a3) {
     return (... thunk_table[207650] ...)(this, a2, a3);
   }
   ```
   At runtime, `g_securom_thunk_table_base+207650` resolves to the
   base-class ctor's real entry point (in the rr01 region) — IDA's
   static view shows only the thunk JMP.

### Conclusion

**The writer of "ControlMapper" into `engine_wilbur+0xCCC` is inside the
SecuROM-thunked base ctor (sub_48D780's resolved target).** Static
analysis cannot reach it. A runtime probe is required.

### Probe design (next concrete step)

Smallest sufficient instrumentation:

1. New module `mtr::coop_registry_probe` (new TU under `src/coop/`).
2. MinHook `sub_5CB420` PRE-entry. In the hook, before the orig:
   - Read `a3` (the key string) — pointer to char*.
   - If `*a3 == 'C' && strcmp(a3, "ControlMapper") == 0`:
     - Read `caller_RA` from `[esp]` (return address — the call site).
     - Log: `this` (registry addr), `caller_RA`, `a2` (value being inserted),
       `a4` (type code), `a5`, `a6`.
   - Forward to orig unmodified.
3. Run `coop-router-soak` scenario; let the engine wilbur init at level load.
4. Read log to capture the "ControlMapper" insert (single capture is enough
   — once we have caller_RA + value + type, we know exactly what to call
   `sub_5CB420` with for the orphan).
5. Decompile the function at `caller_RA - some_offset` to see what other
   inserts neighbour the "ControlMapper" insert — gives us the full set
   of registry keys we'll need to populate on the orphan (likely
   "groundPosition", "onGround", "KinematicController" etc. cluster
   here too).

Estimated work: ~50 LOC + 1 scenario run. Half-day.

### Why not just hook sub_5CB310?

We could also hook the LOOKUP path and redirect orphan→engine wilbur on
NULL result. But that's a hot-path hook fired thousands of times per
frame, and it would hide the proper architecture (each wilbur should
have its own registry). The insert-side probe is one-shot and points us
at the exact engine code we need to mirror — true Rule №1 path.

### Build artifact

Still no code change. Probe module to be implemented next turn.

---

## Phase 2 step (b7.1-next2) — coop_registry_probe SHIPPED + writer captured

**Date**: 2026-05-11 (final+8)

**Status**: PROBE SHIPPED. Writer captured + decoded. Engine-side CM-attach
factory fully mapped. Ready to design orphan-fix.

### Implementation

New TU `src/coop/coop_registry_probe.cpp` + header
`include/mtr/coop_registry_probe.h`. Hook: MinHook PRE-trampoline on
`sub_5CB420` with signature `__fastcall(this, edx, value, key_str, type,
flag1, flag2)` matching IDA's `__thiscall(_DWORD*, int, const char*, int,
int, int)`. Fast-path filter: `key == &kControlMapperName[0x6A6EE0]`, fallback
SEH-guarded `strcmp` for indirect refs. Wired in `dllmain.cpp` after
`coop_component_registry::install()`. Build: 703488 B (+1536 B from previous
701952 B). Pure addition, zero behavior change to existing modules.

### Capture from load-save-1-show-ingame scenario

```
[coop_reg_probe] FIRST ControlMapper insert:
  registry=0x0EFBACAC  ra=0x0051F58D  value=0x0EFB9FE0  type=5
  flag1=0x00000000  flag2=0x00000000  match=direct  total=1
```

Critical observations:
- `registry - 0xCCC = 0x0EFB9FE0 = value` — **the writer passes the wilbur
  itself as the slot value**, not the ControlMapper. The CM goes into
  `*(slot+12)` via a separate `sub_5CB220` call after readback.
- `type=5` (resource type — consistent with `sub_5CB420`'s `case 5` path that
  allocates a 4-byte storage cell at `slot+12`).
- Single insert per scenario run (1869 frames, 1 wilbur fully initialized) —
  confirms one-shot per-wilbur, not per-frame.

### Writer decoded — function at 0x51F4D0 (rr01 unanalyzed region)

IDA's `define_func` rejects this region (SecuROM-decompressed at runtime,
mixed with padding). Decoded from raw bytes:

```asm
0x51F4D0  64 A1 00 00 00 00              mov     eax, fs:[0]              ; SEH prologue
0x51F4D6  6A FF                           push    -1
0x51F4D8  68 D2 B9 69 00                  push    0x69B9D2                 ; SEH handler
0x51F4DD  50                              push    eax
0x51F4DE  64 89 25 00 00 00 00            mov     fs:[0], esp
0x51F4E5  83 EC 18                        sub     esp, 0x18                ; 24B locals
0x51F4E8  53 56                           push    ebx; push esi
0x51F4EA  8B 74 24 30                     mov     esi, [esp+0x30]          ; param: array index
0x51F4EE  8B D9                           mov     ebx, ecx                 ; ebx = this (manager)
0x51F4F0  8B 4C B3 04                     mov     ecx, [ebx+esi*4+4]       ; entity = this.array[esi]
0x51F4F4  57                              push    edi
0x51F4F5  68 4C FE 6B 00                  push    0x6BFE4C                 ; "useTestPlayerControlMapper"
0x51F4FA  81 C1 7C 0C 00 00               add     ecx, 0xC7C               ; ecx = entity + 0xC7C
0x51F500  E8 FB 99 F9 FF                  call    0x4B8F00                 ; entity_kv_get (stolen-byte IAT thunk)
0x51F505  85 C0                           test    eax, eax
0x51F507  74 3A                           jz      0x51F543                 ; if NOT present → default-CM branch

; === IF-branch: entity has useTestPlayerControlMapper → custom CM ===
0x51F509  68 6C 01 00 00                  push    0x16C                    ; size = 364 bytes
0x51F50E  E8 AD 3D 06 00                  call    0x5832C0                 ; malloc
0x51F513  83 C4 04                        add     esp, 4
0x51F516  89 44 24 34                     mov     [esp+0x34], eax          ; save heap ptr
0x51F51A  85 C0                           test    eax, eax
0x51F51C  C7 44 24 2C 00 00 00 00         mov     [esp+0x2C], 0            ; ctor-init flag
0x51F524  74 11                           jz      0x51F537                 ; if alloc failed
0x51F526  8B C8                           mov     ecx, eax
0x51F528  E8 D3 53 01 00                  call    0x534900                 ; CM-subclass ctor
0x51F52D  C7 44 24 2C FF FF FF FF         mov     [esp+0x2C], -1           ; ctor-success
0x51F535  EB 19                           jmp     0x51F550

0x51F537  33 C0                           xor     eax, eax                 ; alloc-fail return null
0x51F539  C7 44 24 2C FF FF FF FF         mov     [esp+0x2C], -1
0x51F541  EB 0D                           jmp     0x51F550

; === ELSE-branch: default CM ===
0x51F543  8B 44 B3 04                     mov     eax, [ebx+esi*4+4]       ; entity
0x51F547  50                              push    eax
0x51F548  E8 03 25 F0 FF                  call    0x421A50                 ; default-CM accessor (SecuROM-thunked)
0x51F54D  83 C4 04                        add     esp, 4

; === CONVERGE: edi = CM instance (custom or default) ===
0x51F550  8B F8                           mov     edi, eax
0x51F552  56 57                           push    esi; push edi
0x51F554  B9 C8 F4 72 00                  mov     ecx, 0x72F4C8            ; global CM hashtable
0x51F559  E8 B2 FF 04 00                  call    0x56F510                 ; insert(this, entry, hash)
0x51F55E  8B 17                           mov     edx, [edi]               ; CM vtable
0x51F560  56                              push    esi
0x51F561  8B CF                           mov     ecx, edi
0x51F563  FF 52 04                        call    [edx+4]                  ; CM.vtable[1] (class-name?)
0x51F566  56 57                           push    esi; push edi
0x51F568  B9 C8 F4 72 00                  mov     ecx, 0x72F4C8            ; same global
0x51F56D  E8 9E FB 04 00                  call    0x56F110                 ; secondary register (SecuROM-thunked)

; === Insert ControlMapper slot into entity+0xCCC registry ===
0x51F572  8B 44 B3 04                     mov     eax, [ebx+esi*4+4]       ; entity
0x51F576  6A 00                           push    0                        ; a6=0
0x51F578  6A 00                           push    0                        ; a5=0
0x51F57A  6A 05                           push    5                        ; a4=type=5
0x51F57C  68 E0 6E 6A 00                  push    0x6A6EE0                 ; a3="ControlMapper"
0x51F581  50                              push    eax                      ; a2=entity (value=owner backref)
0x51F582  8D 88 CC 0C 00 00               lea     ecx, [eax+0xCCC]         ; this = entity+0xCCC
0x51F588  E8 93 BE 0A 00                  call    sub_5CB420               ; ← OUR PROBE FIRES HERE

; === Write CM instance into the slot's storage cell ===
0x51F58D  8B 44 B3 04                     mov     eax, [ebx+esi*4+4]       ; entity
0x51F591  6A 00                           push    0
0x51F593  68 E0 6E 6A 00                  push    0x6A6EE0                 ; "ControlMapper"
0x51F598  8D 88 CC 0C 00 00               lea     ecx, [eax+0xCCC]
0x51F59E  E8 6D BD 0A 00                  call    sub_5CB310               ; read slot back
0x51F5A3  85 C0                           test    eax, eax
0x51F5A5  74 08                           jz      0x51F5AF
0x51F5A7  57                              push    edi                      ; CM instance
0x51F5A8  8B C8                           mov     ecx, eax                 ; ecx = slot
0x51F5AA  E8 71 BC 0A 00                  call    sub_5CB220               ; *(slot+12) = CM
0x51F5AF  8B 44 B3 04                     mov     eax, [ebx+esi*4+4]
0x51F5B3  50                              push    eax
0x51F5B4  E8 17 2B F0 FF                  call    0x4420D0                 ; post-install hook (rr01 — undecompilable)
0x51F5B9  8B 83 C4 03 00 00               mov     eax, [ebx+0x3C4]         ; controller-array on manager
; ... (truncated)
```

### Key strings + cross-checks

- **`0x6BFE4C`** = ASCII `"useTestPlayerControlMapper"` (verified via
  `get_bytes`). Adjacent strings: `"updateLevel"`, `"holoPrometheus"`,
  `"ResetWea..."` — entity-config key cluster.
- **`0x6A6EE0`** = `"ControlMapper"` (already known canonical string ptr).
- **`0x72F4C8`** = global CM hashtable (16-byte slot stride, `slot[0]/slot[4]`
  head/tail, `slot[12]` count). `sub_56F510` links CM into list at
  `hashtable + 16*hash_arg`.

### Sub-functions decoded

#### `sub_534900` — custom ControlMapper subclass ctor

```c
int __thiscall sub_534900(int this) {
    ControlMapper_base_ctor((_BYTE *)this, unk_6C1DA0);  // base vtable
    *(_DWORD *)this = 7085420;                            // = 0x6C1F2C — override vtable
    unk_6C1D8C(this, 0);                                  // base-class init
    (*(void (__thiscall **)(int, _DWORD))(*(_DWORD *)this + 36))(this, 0); // vtable[9]
    *(_DWORD *)(this + 356) = 1056964608;                 // 0.5f
    *(_DWORD *)(this + 360) = 1056964608;                 // 0.5f
    return this;
}
```
Derived ControlMapper subclass with vtable `0x6C1F2C` (overrides base
`0x6C1DA0`). Size 364B. The two 0.5f fields at +356/+360 are likely
deadzone / sensitivity defaults.

#### `sub_5CB220` — slot storage writer

```c
_DWORD *__thiscall sub_5CB220(_DWORD *this, int a2) {
    if ( *(this + 4) == 5 ) {           // slot[16] type-check
        result = (_DWORD *)*(this + 3); // slot[12] = storage cell
        *result = a2;                    // *storage = CM instance
    }
    return result;
}
```
Type-gated: only writes if slot is type-5 (resource). Storage cell was
allocated by `sub_5CB420`'s `case 5` path during the insert.

#### `sub_421A50` — default-CM accessor

```c
int __thiscall sub_421A50(void *this, int a2, int a3) {
    return ((int (__thiscall *)(...))(&g_securom_thunk_table_base + 208462))(...);
}
```
SecuROM thunk forwarder — body in rr01 runtime-decompressed region.
Returns the engine's pre-existing default ControlMapper for entities
without `useTestPlayerControlMapper`.

#### `sub_56F510` — global CM hashtable insert

```c
void __thiscall sub_56F510(char *this, _DWORD *a2, int a3) {
    if (a2) {
        v4 = 0;
        if (unk_72F820+76 routine(a3))                // hash-validate
            v4 = unk_72F820+60 routine(a3);            // canonicalize hash
        a2[1] = v4;
        sub_56E780(a2);                                // pre-link init
        v5 = this + 16 * a3;                           // slot at hash*16
        if (*v5) {                                     // chain to existing
            a2[88] = 0; a2[87] = v5[1];               // a2.prev=tail, a2.next=null
            *(v5[1] + 352) = a2;                       // old_tail.prev_of_352 = a2
            v6 = v5[3] + 1;
            v5[1] = a2;                                // new tail
            v5[3] = v6;                                // count++
        } else {                                       // empty slot
            v5[1] = a2; *v5 = a2;
            a2[88] = 0; a2[87] = 0;
            v5[3]++;
        }
    }
}
```
Doubly-linked-list registration. `a2[87]/a2[88]` at offsets +348/+352 in the
CM = prev/next. **The CM tracks its own list-position; the chain is reachable
from `0x72F4C8 + 16*hash`.**

### What we now know vs what we need for orphan-fix

| Known | Status |
|---|---|
| Per-wilbur registry layout (+0xCCC) | ✓ Solid (verified by both readers and writer) |
| CM-attach factory @ 0x51F4D0 | ✓ Decoded |
| Default vs custom CM dispatch | ✓ Gated on `useTestPlayerControlMapper` kv |
| Custom CM subclass: 364B, vtable 0x6C1F2C, ctor 0x534900 | ✓ |
| Global CM hashtable @ 0x72F4C8, linked via +348/+352 | ✓ |
| Slot-storage writer sub_5CB220 | ✓ |
| Post-install hook 0x4420D0 (rr01-thunked) | ✗ Body opaque — likely controller-binding |
| Manager pointer (the `ebx` value at 0x51F4D0 entry) | ✗ Need a 2nd probe or stack-walk |
| Array index (the `esi` value) | ✗ Same |

### Path forward — Rule №1 (proper, no crutches)

**Option B (recommended)**: Reuse the engine's CM-attach by calling
**0x51F4D0** itself with the orphan added to the manager's array. Requires
finding the manager. Manager identification path:

1. The manager has the wilbur-array at `[this+esi*4+4]` (i.e. `this[1+esi]`).
2. The manager also has something at `this+0x3C4` (touched on line 0x51F5B9).
3. The manager's pointer is reachable from the CM by walking
   `0x72F4C8 + 16*hash[engine_wilbur]` — once we have the engine wilbur's CM,
   we can walk back through the post-link state to find the owner. But
   simpler: hook 0x51F4D0 entry once and log `ebx`.

**Option A (alternative)**: Mini-replay the constituent calls of 0x51F4D0
manually on the orphan. We have all the pieces:
- `malloc(0x16C); sub_534900(it)` — new CM
- `sub_56F510(0x72F4C8, CM, hash_of_orphan)` — register globally
- vtable[1] call on CM (probably class-name canonicalization)
- `sub_56F110(0x72F4C8, CM, hash)` — secondary register (thunked, opaque)
- `sub_5CB420(orphan+0xCCC, orphan, "ControlMapper", 5, 0, 0)` — insert slot
- `slot = sub_5CB310(orphan+0xCCC, "ControlMapper", 0)`
- `sub_5CB220(slot, CM)` — wire CM into slot storage
- `sub_4420D0(orphan)` — post-install hook (opaque but callable)

Risk in Option A: we don't know what `vtable[1]` does (might require manager
context); `sub_56F110` is thunked-opaque and may need state we don't have;
`sub_4420D0` is thunked-opaque and may bind controllers we haven't set up.

**Recommendation**: Add a second probe on `0x51F4D0` entry to log `ebx`
(manager) and `esi` (index), then choose between Options A and B from solid
data, not speculation. **Step (b7.1-next4): add 2nd probe; then decide.**

### Build artifact (this step)

`mtr-asi.asi` = **703,488 bytes** (+1536 B from previous 701,952 B). Probe is
PRE-trampoline only; no behavior change. Scenario `load-save-1-show-ingame`
passes (PASS, 1869 frames, 8.7s).

---

## Phase 2 step (b7.1-next4) — Manager identified: `g_player_controller_mgr` @ 0x00728A40

**Date**: 2026-05-11 (final+9)

**Status**: MANAGER IDENTIFIED via inline-asm EBX/ESI capture in the existing
`sub_5CB420` hook. Cross-check passes. The orphan-fix pathway is now fully
specified — no further RE blockers.

### Failed attempt: direct hook on 0x51F4D0

First attempt: install a 2nd MinHook PRE-trampoline on the writer function
entry at `0x51F4D0` itself, declared as `__fastcall(this, edx, a1, a2, a3,
idx)` (assuming 4 stack args, with idx at `[esp+0x30]` translating to arg4).

Result: AV crash 6 seconds after probe-armed log. Captured args were
garbage:
```
manager=0x00728A40 idx=259046656 a1=0 a2=0x76EAB200 a3=0x76E945B0
wilbur=[manager+idx*4+4]=0xDEADBEEF ra=0x022D8056
```
- `idx=259046656` (=`0x0F702400`) — absurdly large for an array index
- `ra=0x022D8056` — not in Wilbur.exe's .text segment
- `a2/a3` look like kernel32/ntdll addresses
- `manager=0x00728A40` — only this looked plausible

Diagnosis: **`0x51F4D0` has zero static xrefs** (per IDA's
`xrefs_to`/`find` queries). The bytes there form what looks like a classic
`mov eax,fs:[0]; push -1; push HANDLER; ...` SEH prologue but it's NOT a
function entry — it's an inner `__try` block prologue of some larger
function whose entry is elsewhere (likely SecuROM-thunked at runtime). Our
hook landed in code reached only via runtime dispatch with a totally
different ABI than a normal `__thiscall`. MinHook installed cleanly but the
caller didn't follow the conventions we assumed.

**Lesson**: in the rr01 runtime-decompressed region, "looks like a prologue"
is necessary but not sufficient. Without a static xref to confirm the
function is entered as a `call` target, MinHook detours can land in dead
code.

### Successful approach: caller-frame register snapshot

Replaced the failed direct hook with an **inline-asm capture inside the
already-working `sub_5CB420` hook**: when the writer at `0x51F588` calls
`sub_5CB420("ControlMapper", ...)`, EBX holds the writer's `manager` and ESI
holds the writer's `idx` (both callee-saved by C convention, both
unmodified by the writer prior to the call site). At the very first
instruction of our hook body — before MSVC's compiler-generated prologue
can clobber EBX/ESI — we snapshot them:

```cpp
uint32_t saved_ebx = 0, saved_esi = 0;
__asm {
    mov saved_ebx, ebx
    mov saved_esi, esi
}
```

Cross-check: compute `*(saved_ebx + saved_esi*4 + 4)` (the writer's exact
expression for the wilbur pointer) under SEH and compare to the captured
`value` arg. If equal, our assumption is validated.

### Result (load-save-1-show-ingame scenario, PASS, 1872 frames, 9.7s)

```
[coop_reg_probe] FIRST ControlMapper insert:
  registry=0x0EE94CAC  ra=0x0051F58D  value=0x0EE93FE0  type=5  match=direct
  writer_ebx=0x00728A40  writer_esi=0
  [ebx+esi*4+4]=0x0EE93FE0  (consistent)
```

**Cross-check PASSES**: `[0x00728A40 + 0*4 + 4] = 0x0EE93FE0 = value`. The
manager pointer is real, the array index is correct.

### Architecture insight: manager is a static engine global

`0x00728A40` lies inside Wilbur.exe's .data/.bss segment (~0x720000-0x740000
range, per existing memory entries for cvar globals at 0x745140/0x745240).
This is a **static singleton** — not a heap-allocated controller, not a
per-level object. We do **not need** to find it dynamically; it's a
compile-time address.

Layout of the manager:
- `manager + 0x000` — (unused header? size? something at top)
- `manager + 0x004` — `wilbur_array[0]` (first slot — engine wilbur)
- `manager + 0x008` — `wilbur_array[1]` (second slot — would be P2 here)
- ... (capacity unknown without further RE; not blocking)
- `manager + 0x3C4` — touched by writer at 0x51F5B9 (controller-array? bookkeeping?)
- `manager + 0xC7C` — passed to `entity_kv_get`... wait, that's `entity+0xC7C` (we read from the wilbur, not the manager). Correction: the kv-gate check is on the WILBUR, not the manager.

### Build artifact (this step)

`mtr-asi.asi` = **704,000 bytes** (+512 B from previous 703,488 B). Inline-asm
capture + helper function + atomics. Pure addition.

### Orphan-fix plan — Option A (minimal-replay) recommended

We do NOT need to reuse the engine's full factory at 0x51F4D0 (it's
unreachable as a normal call target anyway). The minimal-replay path
inserts the CM slot directly into the orphan's +0xCCC, pointing at the
**engine's existing CM** (which we read from the engine wilbur's slot):

```cpp
// 1. Read the engine wilbur's CM out of its registry.
uint32_t engine_wilbur = *(uint32_t*)(0x00728A40 + 4);  // manager.array[0]
uint32_t engine_registry = engine_wilbur + 0xCCC;
uint32_t engine_slot = sub_5CB310((void*)engine_registry,
                                  (const char*)0x6A6EE0, 0);
uint32_t engine_cm = *(uint32_t*)(engine_slot + 0xC);   // slot.storage[0]

// 2. Insert the same CM into orphan's +0xCCC.
uint32_t orphan_registry = orphan + 0xCCC;
sub_5CB420((void*)orphan_registry,
           orphan,                    // value = owner backref (per engine pattern)
           (const char*)0x6A6EE0,     // "ControlMapper"
           5,                          // type = resource
           0, 0);                      // flags
uint32_t orphan_slot = sub_5CB310((void*)orphan_registry,
                                  (const char*)0x6A6EE0, 0);
sub_5CB220((void*)orphan_slot, engine_cm);  // *storage = engine CM
```

After this, `sub_5CB310(orphan+0xCCC, "ControlMapper", 0)` returns a valid
slot, `sub_5CB160(slot)` reads `slot+4=1` (ref-flag set by sub_5CB420), AV
gone. ViewDriver.vtable[13] runs to completion.

**Risks/caveats**:
- We do NOT register orphan's CM in the global hashtable at `0x72F4C8`. Engine
  code that iterates this list (e.g. for input dispatch) won't see the orphan.
  But orphan input goes through b2-rem-2 component thunks, NOT through this
  global list — so no functional regression for our use case.
- The engine's CM is shared between engine wilbur and orphan. Input arrives
  via b2-rem-2 thunks which set `instance+4 = dev_P1 or dev_P2` based on the
  component's player_idx. Engine wilbur's components route to dev_P1; orphan's
  to dev_P2. Correct routing maintained.
- When engine wilbur dies (e.g. level change), the CM may be freed → orphan's
  slot becomes dangling. Mitigation: hook engine wilbur destruction to also
  clear orphan's slot. Defer until live test confirms the basic fix works.

### Next: (b7.2) Implement orphan-fix in coop_spawn_probe

Add a post-spawn step after `try_spawn_p2()` returns the orphan pointer:
1. Read engine_wilbur via `*(uint32_t*)0x00728A40 + 4`.
2. Lookup engine CM via `sub_5CB310(engine_wilbur+0xCCC, "ControlMapper", 0)`,
   read `slot+12` storage cell.
3. Insert into orphan registry via `sub_5CB420` + `sub_5CB220`.
4. Run keep-alive scenario; verify no AV at 0x5CB163.

Estimate: ~50 LOC + 1 scenario run. ~1 hour.

---

## Phase 2 step (b7.2/b7.3) — Orphan-fix SHIPPED + tested. ControlMapper-NULL class FIXED. Next crash class identified.

**Date**: 2026-05-11 (final+10)

**Status**: SHIPPED + LIVE-TESTED. Build = **705,536 bytes**. Cmdline-gated by
`-mtrasi-coop-keep-orphan` (off by default — existing teardown path intact).

### Implementation

`coop_spawn_probe.cpp` gained:
- New function `attach_engine_cm_to_orphan(void* orphan)` (~75 LOC, SEH-wrapped).
- 3 engine-fn typedefs + 5 VA constants (sub_5CB310/sub_5CB420/sub_5CB220,
  ControlMapper string, player-controller manager).
- Cmdline gate via `keep_orphan_enabled()` static-initialized lambda.
- Modified teardown block: when keep-orphan ON, call `attach_engine_cm_to_orphan`
  and SKIP the `vtable[0](orphan, 1)` destruction. Default behavior unchanged.

### Live test result (scenario load-save-1-show-ingame + `-mtrasi-coop-keep-orphan`)

```
21:57:32.211 try_spawn_p2 fires (orphan = 0x0EF639E0)
21:57:32.452 factory POST: return=0EF639E0 (call #4 SUCCESS)
21:57:32.452 STEP2J VEH FAULT eip=0x5375EC03 av_addr=0x4AD33B14 (NEW CRASH CLASS)
21:57:33.165 [coop_reg_probe] NEW-RA ControlMapper insert ra=0x5375923E total=2
              (this is OUR ATTACH calling sub_5CB420 — RA is inside our ASI)
21:57:33.165 attach_engine_cm SUCCESS:
              engine_wilbur=0x0EFCCFE0 engine_cm=0x0EFA2060
              orphan=0x0EF639E0 orphan_slot=0x0EF414C0
              orphan_storage=0x0EF41480 *storage=0x0EFA2060 (wired)
21:57:33.165 B7.2 keep-orphan path: attach_engine_cm_to_orphan=OK
              — SKIPPING teardown. Orphan will live;
              observe next ~60 frames for AV.
```

**Then**: process terminated with exit code -1073741819 (0xC0000005). No "result
JSON" written → harness reported launch failure.

### Verdict — TWO findings

**Finding 1: ControlMapper-NULL crash class is GONE.**

The prior keep-alive crash chain
`ViewDriver.vtable[13] → sub_5CB310(orphan+0xCCC, "ControlMapper", 0) → NULL → sub_5CB160(NULL) → AV at 0x5CB163`
NO LONGER fires. After our attach, `sub_5CB310(orphan+0xCCC, "ControlMapper", 0)`
returns the orphan's slot, `slot+12 = orphan_storage`, `*orphan_storage = engine_cm`,
`sub_5CB160` reads `*(engine_cm + 4)` successfully — no AV.

**The orphan lived ~700ms past the prior crash point** (previously crashed ~150ms
post-spawn). Big keep-alive improvement.

**Finding 2: NEW crash class at 0x5375EC03 (inside OUR ASI's mapped image).**

VEH captured an AV at `eip=0x5375EC03`, in the range `0x53700000-0x53800000`
where our ASI is mapped (confirmed by boot log line
`sprite_probe: call-site patched at 004D23BF (...) wrapper=53746200`). The
crash is in our OWN POST-hook chain or `attach_engine_cm_to_orphan`,
NOT in engine code.

Crash state:
- `eax = 0x0EFA2060` (engine_cm — set by attach_engine_cm earlier)
- `ecx = 0x0EF646AC` (orphan_registry = orphan+0xCCC)
- `edi = 0x0EF639E0` (orphan)
- `av_addr = 0x4AD33B14` (read fault — high address, possibly DXVK or system DLL)

Likely the VEH was captured DURING `attach_engine_cm`'s call chain — perhaps
the `sub_5CB420`/`sub_5CB310` calls into the engine touched some state that
required the orphan to be more-fully-initialized than it is. Or a parallel
sim-tick thread tried to do something with the orphan that conflicted.

**Key observation**: the engine ITSELF auto-attaches CMs to orphans! The
`[coop_reg_probe] NEW-RA` line was originally observed as having `ra=0x5375923E`
— at first I thought this was the engine, but on closer inspection the RA is
inside our ASI's mapped range. So this RA is from OUR call site to sub_5CB420
inside `attach_engine_cm_to_orphan`. The engine does NOT auto-attach a CM to
the orphan — only the engine_wilbur insert (from 0x51F58D) was captured for
the engine. So our explicit attach IS necessary.

### Followup-side artifact cleanup

The cross-check log line in `coop_registry_probe.cpp` shows
`[ebx+esi*4+4]=0xDEADBEEF (MISMATCH)` for the NEW-RA hit. This is expected:
the EBX/ESI captured at the start of our hook reflect MY caller's register
state (i.e. inside `attach_engine_cm_to_orphan`'s local frame), not the
engine writer's. The cross-check only makes sense for `ra==0x0051F58D`.
Minor cosmetic issue, doesn't affect functionality.

### Build artifact (this step)

`mtr-asi.asi` = **705,536 bytes** (+1536 B from 704,000). Includes the orphan-fix
function + cmdline gate.

### Next: (b7.5) Diagnose 0x5375EC03 crash in POST-hook chain

The remaining crash is in our own POST-hook code path, downstream of factory
return. Address 0x5375EC03 is inside our ASI. Approaches:
1. Add a crash_handler module-list dump that maps `0x5375EC03` → exact file:line
   in the ASI build (PE PDB symbol resolution).
2. Add aggressive logging in the POST-hook chain to bracket where the fault
   occurs (between which two log lines does VEH fire?).
3. Reduce keep-orphan path to MINIMAL — comment out attach_engine_cm and run
   again to see if the crash still occurs (would isolate whether the crash
   is in our attach or in the underlying engine call we make).

Estimate: ~2-4 hours to localize. Then either fix or work around.

### Closing this session

The b7.2 milestone — orphan CM-attach — is SHIPPED and the keep-alive crash
class it targeted is GONE. The orphan now persists past the prior failure
point. The next crash class is downstream and inside our own code, which is
easier to diagnose than engine-side rr01-thunked crashes were. Session ends
with substantial forward progress: from "orphan crashes ~150ms post-spawn"
to "orphan lives ~700ms post-spawn, new bug is in our code".

---

## Phase 2 step (b7.5) — RETRACTION: crash was NOT in our code

**2026-05-11 (final+11 — this session.)** Status: **diagnosed.** Build = 707,072 B.

The (b7.4) hypothesis ("the new crash at 0x5375EC03 is in our own POST-hook
chain") was WRONG. After enabling PDB-backed symbol resolution and N-shot
VEH logging, the actual crash chain is:

1. **Cosmetic noise (NOT fatal):** Two SEH-guarded probes (`coop_registry_probe::read_writer_wilbur_seh`
   and `controlmapper_probe::read_dword_seh`) deliberately deref possibly-invalid
   memory under `__try/__except`. Both AVs are caught and returned as 0 / 0xDEADBEEF.
   Their VEH log lines were misleading us into thinking the crash was in our code.

2. **The actual fatal AV** is inside the engine, at the indirect call following
   the ControlMapper lookup in `sub_5454B0` (ViewDriver.vtable[13]). Specifically:
   - sub_5454B0 calls `sub_5CB310(orphan+0xCCC, "ControlMapper", 0)` → returns
     our wired slot (non-NULL ✓)
   - sub_5454B0 then calls an indirect (vtable or function-pointer) call at
     RA `0x005454F5` (= sub_5454B0 + 0x45)
   - The target of that call is `0x00000002` → CPU faults trying to fetch
     instructions at address 2

Symbolicated VEH trace (last fire is fatal):
```
STEP2J VEH FAULT #2 code=0xC0000005 eip=0x00000002 av_op=0 av_addr=0x00000002
       sym_eip=<unresolved>
STEP2J #2 regs eax=0x10D70060 ebx=0x76E94500 ecx=0x10D70060 edx=0x10D70120
           esi=0x10D2D400 edi=0x10D70060 ebp=0x1170D360 esp=0x030FFD74
STEP2J #2 stack[00..03]=005454F5 00000003 10D31A20 10D2DD0C
STEP2J #2 stack[04..07]=00000003 005EBA8E 3D889A02 10D31A20
STEP2J #2 stack[08..11]=10D2DD24 10D2F440 005AD9DC 10F512D0
STEP2J #2 stack[12..15]=005C85D5 1134A550 1134A530 005AD646
```

Decoded call chain (from deepest):
- `entity_manager_tick_components` (sub_5AD4D0) at offset 0x176 calls
  vtable[1] on entity_manager+32 → RA `0x5AD646`
- That callee (around 0x5C85D5) calls sub_5AD9B0 → RA `0x5AD9DC`
- `sub_5AD9B0` walks linked list at `this+0xD04` (this = entity_manager+32,
  ie. ViewDriver subsystem). For each node, calls vtable[13] on `*node`
  (the wilbur-like object in the list). RA `0x5AD9DE` (slight offset).
- vtable[13] = sub_5454B0 (= ViewDriver.vtable[13])
- Inside sub_5454B0 at offset 0x45: indirect call with target `0x00000002`

Crash regs decode:
- `ecx = 0x10D70060` = engine_cm = engine's ControlMapper (we wired this into
  orphan's +0xCCC ControlMapper slot via the b7.2 attach)
- `edx = 0x10D70120` = engine_cm[0] (first dword of engine_cm — alleged
  vtable pointer)
- `eip = 2` = the actual call target was 0x2

The call instruction inside sub_5454B0 is either `call [ecx+N]` or
`call [edx+N]` where that slot holds `0x2`. Since the SAME engine_cm
works fine when the engine ticks engine_wilbur (which has its OWN slot
in its own +0xCCC registry pointing at this SAME engine_cm), the
problem is NOT engine_cm being corrupted. The most likely root causes:

- **A**: sub_5454B0 reads OTHER fields off the orphan (besides the CM
  lookup) — eg. another per-wilbur registry key, a transform pointer, a
  scene-graph link — and one of those fields is uninitialized (0 or stack
  garbage) on the orphan, leading the engine to call a function pointer
  that resolves to 0x2.

- **B**: sub_5454B0 dispatches via `call [edx + idx*4]` where `idx` is
  computed from a field on the orphan. With orphan's field uninitialized,
  `idx` lands on a vtable slot that genuinely holds 0x2 (perhaps a
  "padding" or "destroyed" sentinel).

### Diagnostic infrastructure added this step (KEEPS)

- **`crash_handler::resolve_symbol(addr, out, sz)`** — public dbghelp wrapper
  (SymInitialize at install time with `INVADE_PROCESS=TRUE` + `SYMOPT_LOAD_LINES`).
  Returns `"function+0xNN  (file:line)"` for any address inside our ASI when the
  PDB is reachable. Falls back gracefully when symbol info is unavailable.
- **`top_level_filter` now emits sym=...** when the crash is in our own code.
- **VEH (coop_spawn_probe::veh_crash_logger)** also calls `resolve_symbol` for
  both eip AND av_addr; the log line now reads `sym_eip=...` and (when av_addr
  resolves to our code) `sym_avaddr=...`.
- **VEH changed from one-shot to N-shot** (cap = 32). Critical: SEH-guarded
  helpers deliberately throw AVs that get caught — the old one-shot burned on
  the first cosmetic AV and missed the fatal one. With N-shot, the LAST entry
  in the log is reliably the fatal AV.
- **CMake** now emits `mtr-asi.map` (linker `/MAP /MAPINFO:EXPORTS`) so RVA
  lookups can also be done offline against the build artifact without dbghelp.
- **`run-test.ps1 -Redeploy`** now also copies `mtr-asi.pdb` next to the
  deployed `.asi` so dbghelp at runtime can find symbols even if the build
  directory is wiped.
- **coop_registry_probe cosmetic gate (b7.5 cosmetic followup):** the
  `[ebx+esi*4+4]` cross-check now only fires when `ra == 0x0051F58D` (the
  engine writer's call site). For all other call sites EBX/ESI hold our
  compiler's local register state, not manager/idx — the deref was a wild
  pointer read producing harmless-but-noisy SEH-caught AVs.

### Confirmation that b7.2 is intact

The keep-alive attach still works correctly:
```
attach_engine_cm SUCCESS: engine_wilbur=0x10D9AFE0 engine_cm=0x10D70060
  orphan=0x10D31A20 orphan_slot=0x10D0F500 orphan_storage=0x10D0F4C0
  *storage=0x10D70060 (wired)
```
And the engine then successfully looks up the wired CM:
```
[cm_probe] NEW via_310 ... outer=0x10D326EC slot=0x10D0F500 kind=5
  storage=0x10D0F4C0 inst=0x10D70060 vt=0x10D70120 ra=0x005454E3
```
ra=`0x005454E3` is inside sub_5454B0 — confirming the engine got past
the ControlMapper-NULL crash (b7.1) and got into the FOLLOWING per-wilbur
work (which b7.5 reveals as missing the rest of the per-wilbur setup).

### Trace artifact

Full VEH/log trace from the b7.5 test run is captured at
`research/findings/coop-phase0-b75-veh-trace-2026-05-11.log` (178 KB).

### Next: (b7.6) Localize the function pointer holding 0x2

sub_5454B0 lives in `rr01` (SecuROM stolen-byte section); IDA shows raw bytes,
not decoded code. Three options to find which field/vtable slot contains 0x2:

1. **Hook sub_5454B0 PRE** and dump all reachable pointers from `this` (the
   wilbur-like object passed in) + engine_cm contents + engine_cm's "vtable"
   contents. Compare orphan's snapshot vs engine_wilbur's. Anything that differs
   is a candidate for the missing initialization.
2. **Dump the raw runtime bytes of sub_5454B0** (offsets 0x00..0x60) via a
   side process or in-mod log so we can decode the instructions and see exactly
   which `call [reg+offset]` is at 0x5454F0. Once we know the offset, we know
   which structure field needs to be valid.
3. **Decompose engine_wilbur's initialization at level-load** to enumerate
   every field/slot the engine writes. Cross-reference against what
   `entity_factory_construct` writes on the orphan to find the gap. Heavy RE,
   probably 1-2 days.

Recommended order: try (1) first (~2 hours), then (2) if needed (~2 hours), only
fall back to (3) (1-2 days) if both fail.

### Build artifact (this step)

`mtr-asi.asi` = **707,072 bytes** (+1536 B from 705,536). Includes:
- `crash_handler::resolve_symbol` + SymInitialize
- VEH N-shot upgrade
- `coop_registry_probe` cosmetic-followup gate
- /MAP linker flag (produces 610 KB `mtr-asi.map` next to `.asi`)

---

## Phase 2 step (b7.6) — Off-by-one-deref root cause IDENTIFIED + FIXED

**2026-05-11 (final+12 — this session, continuation of b7.5.)**
Status: **SHIPPED, validated, new crash class downstream.**
Build = **710,144 B**.

### What we built

New module `viewdriver_tick_probe.cpp` (~250 LOC) hooks sub_5454B0 PRE-entry and:
- **One-shot dumps 0x180 raw bytes** of the function so the post-CM-lookup
  call sequence can be decoded offline (rr01 thunked region — no static
  decompile available).
- **Per-`this` rate-limited snapshot** (cap=3 hits per distinct wrapper,
  up to 8 wrappers tracked) of `this` state (56 dwords) — bypasses the
  starvation-by-engine_wilbur-volume problem the initial cap=32 design hit.
- **Replicates the engine's CM resolution chain** (sub_5CB310 lookup +
  storage-cell double-deref) inside the PRE hook so we can dump what the
  engine WILL find: `cm_inst`, `cm_vt`, `cm_vt[4]`, and the whole vt[0..7].
  Labels the result `(== 0x2 — WILL CRASH)` or `(valid)`.
- **Wraps `this` classification** as ENGINE (when `this+4 == engine_wilbur`)
  vs OTHER (= orphan / unexpected) so the comparison is visible at a glance.

### What the probe revealed

Side-by-side dump from the test run:

**engine_wilbur tick (works):**
```
[vdt_probe] CALL ENGINE: this=0x10DD5460 wrapped_wilbur=0x10DD9FE0 ...
  cm: wilbur=0x10DD9FE0 registry=0x10DDACAC cm_inst=0x10DAF120
      cm_vt=0x006A639C cm_vt[4]=0x0056E8D0 (valid)
  cm_vt @ 0x006A639C: vt[0]=0x006914F0 vt[1]=0x004F92A0 vt[2]=0x0059AAF0
                     vt[3]=0x0056EEC0 vt[4]=0x0056E8D0 vt[5]=0x0056E940
                     vt[6]=0x0056E9C0 vt[7]=0x0056EA40
```
All vtable slots are real engine code pointers (0x4-0x6 RVA range).

**orphan tick BEFORE b7.6 fix (crashes):**
```
[vdt_probe] CALL OTHER: this=0x10D6C400 wrapped_wilbur=0x10D70A20 ...
  cm: wilbur=0x10D70A20 registry=0x10D716EC cm_inst=0x10DAF060
      cm_vt=0x10DAF120 cm_vt[4]=0x00000002 (== 0x2 — WILL CRASH)
  cm_vt @ 0x10DAF120: vt[0]=0x006A639C vt[1]=0x117549E0 vt[2]=0x00000001
                     vt[3]=0x00000001 vt[4]=0x00000002 vt[5]=0x00000003
                     vt[6]=0x00000004 vt[7]=0x00000000
```
`cm_inst=0x10DAF060` is OFF by exactly 0xC0 from the real `cm_inst=0x10DAF120`.
The "vtable" 0x10DAF120 is **the actual engine_cm instance itself** —
because the orphan's storage cell holds the wrong value (engine's
storage_cell_address, not the CM instance pointer). Reading vt[4]
from that bogus "vtable" reads engine_cm's field at +0x10 — which
contains uninitialized/sentinel `0x00000002`.

### Root cause

In `attach_engine_cm_to_orphan` (coop_spawn_probe.cpp), the engine_cm
extraction was **one-level-off**:

```cpp
uint32_t engine_cm = *reinterpret_cast<uint32_t*>(engine_slot + 0x0C);
//                  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//                  one deref — yields storage_cell_address, NOT the
//                  CM instance pointer
```

The engine's slot-resolution chain (per `sub_5CB160` decompile) is
**double-deref**: `cm = *(*(slot+0x0C))`. The first deref gets the
storage cell address (e.g. 0x10DAF060); the second deref reads the
CM instance pointer out of that cell (e.g. 0x10DAF120).

Confirmed by IDA:
```c
// sub_5CB160 (slot resolver)
if (*(this + 4) == 5)
    return *(_DWORD *)*(this + 3);   // **(slot + 0xC)
```

And `sub_5CB220` (storage writer):
```c
// sub_5CB220(slot, value): writes value INTO the cell, doesn't replace cell ptr.
if (*(this + 4) == 5) {
    result = *(this + 3);            // cell address
    *result = a2;                    // *cell = value
}
```

So my `sub_5CB220(orphan_slot, /*value=*/engine_storage_cell_addr)` wrote
the engine's storage cell ADDRESS into the orphan's cell. When the engine
later did `**(orphan_slot + 0xC)` to get the CM instance, it found the
storage cell address treated as a CM pointer — and `*(that+0x10)` was
engine_cm's own field at +0x10, which happened to be `0x00000002`.

### The fix (1 extra deref)

```cpp
uint32_t engine_storage_addr =
    *reinterpret_cast<uint32_t*>(engine_slot + 0x0C);
uint32_t engine_cm =
    *reinterpret_cast<uint32_t*>(engine_storage_addr);
// ... pass engine_cm (the real instance ptr) to sub_5CB220
```

Verified post-fix log:
```
attach_engine_cm SUCCESS: engine_wilbur=0x10DF1FE0
  engine_storage=0x10DC7060 engine_cm=0x10DC7120
  orphan=0x10D889E0 orphan_slot=0x10D664C0
  orphan_storage=0x10D66480 *storage=0x10DC7120 (wired)
```

`engine_cm=0x10DC7120` is now the real CM instance (not the cell address).

And vdt_probe on the post-fix orphan tick:
```
cm: wilbur=0x10D889E0 registry=0x10D896AC cm_inst=0x10DC7120
    cm_vt=0x006A639C cm_vt[4]=0x0056E8D0 (valid)
```

`cm_inst` and `cm_vt` now match engine_wilbur's values exactly. The
b7.5 `call to 0x2` crash class is GONE.

### New crash class (b7.7 to chase next)

After the b7.6 fix, the orphan now survives the CM dispatch in sub_5454B0
but crashes ~14ms later in a DIFFERENT engine path:

```
STEP2J VEH FAULT #2 code=0xC0000005 eip=0x0058D331 av_op=0
       av_addr=0x00000050 sym_eip=<unresolved>
#2 regs: eax=0x00745B70 ebx=0x76E94500 ecx=0x00000000 edx=0x00728A48
         esi=0x10D85C50 edi=0x00728A40 ebp=0x11764360 esp=0x030FFD7C
#2 stack[00..03] = 10D85C50 004CD7F0 006B6AA0 00532B64
#2 stack[04..07] = FFFFFFFF 10D889E0 10D864A8 10D85C50
#2 stack[08..11] = 005AD9DC 10FA82D0 005C85D5 113A1550
#2 stack[12..15] = 113A1530 005AD646 118047C0 11803D00
```

Decoded:
- eip = `sub_58D330+1` (decompile confirms: `v2 = *(this + 20)` = `*(this+0x50)`)
- ecx = NULL → `this` for sub_58D330 is NULL
- av_addr = 0x50 ✓ matches the `[ecx + 0x50]` read

```c
// sub_58D330 — list iterator
int sub_58D330(_DWORD *this, int a2) {
    v2 = *(this + 20);             // <-- CRASH: this is NULL
    if (!v2) return 0;
    while (sub_63599D(a2, v2 + 4)) {
        v2 = *(_DWORD *)(v2 + 72);
        if (!v2) return 0;
    }
    return v2;
}
```

Caller is `sub_4CD7B0` (RA `0x004CD7F0`) — another rr01-thunked function.
That function read NULL for `this` and called sub_58D330(NULL).

Stack chain (deepest → shallowest):
- `entity_manager_tick_components` (sub_5AD4D0+0x176)
- `... sub_5C85?? ...` (RA 0x005C85D5)
- `sub_5AD9B0` (list walker, RA 0x005AD9DC)
- `sub_5454B0` (ViewDriver.vtable[13]) — past its CM dispatch
- `... sub_532B?? ...` (RA 0x00532B64)
- `sub_4CD7B0` (RA 0x004CD7F0)
- `sub_58D330(this=NULL)` ← CRASH

**Hypothesis**: the orphan is missing OTHER per-wilbur registry entries
beyond `ControlMapper`. sub_4CD7B0 / sub_532Bxx likely look up a different
key in `orphan+0xCCC`, get NULL, and forward that NULL into sub_58D330.

Decoded bytes from sub_5454B0+0xB0..+0xC0 confirm: there's a SECOND
`sub_5CB310(wilbur+0xCCC, key_at_0x006C4F40, 0)` lookup later in the
function:

```
+0xAE: 8B 46 04           ; MOV EAX, [ESI + 4]            ; wilbur
+0xB1: 68 40 4F 6C 00     ; PUSH 0x006C4F40               ; second key string
+0xB6: 8D 88 CC 0C 00 00  ; LEA ECX, [EAX + 0xCCC]        ; orphan+0xCCC
+0xBC: E8 DF 5D 08 00     ; CALL sub_5CB310               ; lookup
+0xC1: 85 C0              ; TEST EAX, EAX
+0xC3: 74 09              ; JZ +0x9                       ; skip if not found
```

So the orphan's tick path forks based on whether the second key
("`?`" — string at 0x006C4F40 needs to be read offline) is present
in its registry. If absent, the JZ skips the resolver; if present
but the cell is uninitialized, we follow the failing path.

### Approaches for (b7.7)

1. **Read the string at 0x006C4F40** to identify the second key, then
   look up the engine's writer for it the same way we found ControlMapper's
   writer for b7.1-next2. Then add a second `attach_X_to_orphan` similar
   to b7.6.

2. **Decode sub_4CD7B0 by reading its runtime bytes** (just like we did
   for sub_5454B0) to identify what its calling convention is and which
   field it reads NULL from.

3. **Alternative: remove the orphan from sub_5AD9B0's list** instead of
   attempting to feed it a complete per-wilbur init. The list is at
   `entity_manager+32+0xD04`. If we can unlink the orphan from that list
   after construction but before the next sim tick, the engine won't
   iterate it for per-wilbur work.

The probe `viewdriver_tick_probe` is now permanently installed; future
b7.7 work can use its data without further instrumentation.

### What's WORKING after b7.6

1. **Original Phase 1 ControlMapper-NULL crash (sub_5CB163 AV)**: FIXED.
2. **b7.5 `call to 0x2` vtable dispatch**: FIXED (was caused by b7.6 deref bug).
3. **Orphan's CM resolution**: now matches engine_wilbur exactly.
4. **PDB symbol resolver**: works end-to-end (`sym_eip=mtr::...+0xN`).
5. **VEH N-shot logging**: works, captures multiple fires reliably.
6. **viewdriver_tick_probe**: permanently captures engine vs orphan tick state.

### Trace artifact

Full b7.6 fix trace: `research/findings/coop-phase0-b76-fix-trace-2026-05-11.log` (186 KB).

### Build artifact (b7.6)

`mtr-asi.asi` = **710,144 bytes** (+3072 B from 707,072). Includes:
- `viewdriver_tick_probe` (new module)
- b7.6 one-extra-deref fix in `attach_engine_cm_to_orphan`
- Extended `this`-state dump (224 bytes vs prior 96)
- Extended function-body dump (0x180 bytes vs prior 0x80)

---

## Phase 2 step (b7.7) — Bypass works locally; orphan is in multiple iteration paths

**2026-05-11 (final+13 — this session, continuation of b7.6.)** Status: **partial
win + decision point reached.** Build = **710,656 B**.

### What we tried

1. **Identified the per-wilbur keys** that `sub_5454B0` looks up beyond
   ControlMapper: read strings at the runtime addresses pushed onto the
   stack in the decoded function body:
   - `0x006A88E8` = `"WeaponInventory"` (lookup at offset 0x80-ish)
   - `0x006C4F40` = `"aimTurning"` (lookup at offset 0xBC)

   Orphan only has `ControlMapper` wired (b7.6); the engine's tick body
   reaches both of the other lookups, gets NULL slots back, and crashes
   downstream when something dereferences the missing data.

2. **Implemented (b7.7) cmdline-gated tick bypass.** Added flag
   `-mtrasi-coop-bypass-orphan-tick`. When set, `viewdriver_tick_probe`'s
   PRE hook checks `*(this+4)`: if it differs from `engine_wilbur`, the
   hook returns WITHOUT calling the trampoline. Safe because `sub_5AD9B0`
   ignores `vtable[13]`'s return value (decompile confirms `void` cast).

### Test result: bypass FIRES but crash persists

```
attach_engine_cm SUCCESS: engine_wilbur=0x10DD9F60 engine_storage=0x10DAEFE0
  engine_cm=0x10DAF0A0 ... *storage=0x10DAF0A0 (wired)
[vdt_probe] BYPASS #0: skipping trampoline for this=0x10D6C380
  wrapped_wilbur=0x10D709A0 (engine_wilbur=0x10DD9F60)
STEP2J VEH FAULT #2 code=0xC0000005 eip=0x0058D331 av_op=0
  av_addr=0x00000050 sym_eip=<unresolved>
```

The bypass *did* skip the trampoline for the orphan-wrapper. But the
identical `sub_58D330(this=NULL)` crash fired anyway, with the same
stack chain (`sub_5AD9B0` → `sub_532Bxx` → `sub_4CD7B0` →
`sub_58D330`).

### What this tells us

`sub_5AD9B0` is a **generic list walker** that iterates one list per
subsystem and calls `vtable[13]` on each node. There are MULTIPLE
subsystems that use this pattern, each with its own list at
`<subsystem>+0xD04`. `sub_5454B0` is `vtable[13]` for ONE class only
(the ViewDriver wrapper class). Other subsystems have their OWN
`vtable[13]` implementations.

When `sub_5AD9B0` walks the next subsystem's list, it finds an entry
related to the orphan, calls THAT subsystem's `vtable[13]`, and the
chain reaches `sub_4CD7B0 → sub_58D330(NULL)` because that
subsystem's per-wilbur state is also uninitialized for the orphan.

EAX at crash = `0x00745B70` — that's inside the static engine cvar
block (memory note: `[0x745B38..0x745B90]` = LOD/Periphery cvars).
So the failing subsystem is reading per-wilbur data alongside
LOD/scene-cvar globals. Likely a render-time LOD or visibility
subsystem that ALSO has wilbur-per-wrapper bookkeeping.

### Decision point — three structural options

The pattern is clear: every per-wilbur engine subsystem will crash on
the orphan because the orphan lacks the corresponding per-wilbur init
state. Whack-a-mole on individual `vtable[13]` PRE hooks won't scale.

The three structural fixes, in order of cleanness:

1. **Unregister from per-wilbur lists.** Find where the orphan gets
   added to each subsystem's list (the breadcrumb at bc[4a] showed
   `sub_5AD410` was the "register active" call), then run the
   corresponding REMOVE on each list immediately after construction.
   The orphan stays alive in memory but isn't iterated by any
   subsystem. Risk: input routing later needs the orphan to BE on at
   least some lists.

2. **Run the engine's per-wilbur init factory on the orphan.** The
   real CM-attach factory is at `0x51F4D0`; it's runtime-thunked. If we
   can route into it (likely via finding a callable trampoline or
   patching the call site of the engine's normal "construct wilbur"
   path), it would populate ControlMapper + WeaponInventory + aimTurning
   + every other per-wilbur key in one call. Risk: factory may take
   args derived from level-load state that we don't have at
   try_spawn_p2 time.

3. **Skip the orphan in `sub_5AD9B0`'s loop.** Hook `sub_5AD9B0` PRE,
   walk its list, REMOVE any node whose underlying wilbur is the
   orphan, then call the trampoline. This is single-hook + reusable
   across all subsystems that use `sub_5AD9B0`. Cleanest if all
   per-wilbur ticks route through this list walker. Risk: there may
   be subsystems that DON'T use `sub_5AD9B0` (other iteration patterns
   exist).

### Recommended for next session

**Option 3 first** (~2-3 hours): hook `sub_5AD9B0` PRE, filter the list
to skip orphan entries, call the trampoline with the filtered list. If
that ALSO doesn't fully solve it, fall back to **Option 2** (engine's
per-wilbur init factory — biggest risk but most "correct" fix).

### What's PERMANENT now (post-b7.7)

- `attach_engine_cm_to_orphan` two-deref fix (b7.6) — keeps.
- `viewdriver_tick_probe` module — keeps as diagnostic, but it's just
  vt[13] for ONE class; useful for future ViewDriver-specific
  investigations.
- `-mtrasi-coop-bypass-orphan-tick` cmdline flag — keeps; off by default
  so non-coop runs are unaffected.
- `crash_handler::resolve_symbol`, VEH N-shot, .map output, .pdb deploy
  — all permanent diagnostic infra from earlier today.

### Trace artifact

`research/findings/coop-phase0-b77-bypass-trace-2026-05-11.log` (185 KB).

### Build artifact (b7.7)

`mtr-asi.asi` = **710,656 bytes** (+512 B from 710,144). Includes:
- `viewdriver_tick_probe::bypass_orphan_tick_enabled()` cmdline gate
- Trampoline-skip path with diagnostic log line
- All earlier b7.6 fixes intact

---

## Phase 2 step (b7.8) — Orphan keep-alive SHIPPED and PASSING

**2026-05-11 (final+14 — this session, continuation of b7.7.)** Status:
**END-TO-END PASS.** Build = **712,192 B**.

### What we built

New module `coop_orphan_filter.cpp` (~200 LOC) hooks `sub_5AD9B0` PRE
(the engine's generic per-wilbur list walker used by multiple subsystems).
On each invocation:
1. Reads `this+0xD04` (the list head pointer for the current subsystem).
2. Walks the linked list. For each node, reads `*(*node + 4)` = the
   wrapper's wilbur ptr. SEH-guarded throughout.
3. If `wilbur == orphan`, **unlinks the node**: rewrites the slot that
   pointed to it (either `*head` or `prev_node[2]`) to skip it. Saves
   `(slot_addr, node_addr, next_addr)` in a fixed-size array.
4. After all unlinks, calls the original trampoline. The trampoline
   walks the FILTERED list and never sees orphan nodes — so its
   per-wilbur work (whatever that is for each subsystem) safely runs
   only for engine_wilbur.
5. After trampoline returns, **re-links saved nodes in reverse order**
   so the original list ordering is restored exactly.

Public coop_spawn_probe helper added: `void* live_orphan_entity()`
returns the orphan ptr ONLY when the last probe attempt succeeded AND
keep-orphan was active. Cmdline-gated by `-mtrasi-coop-filter-list-walker`.

### Key sizing insight

The orphan's OWN per-wilbur list (at `orphan + 0xD04`) had **40 nodes**.
The initial prototype with `kMaxUnlinks = 4` left 36 orphan-self
subscribers in the list — the trampoline iterated those, called their
`vtable[13]`, and crashed at `sub_58D330(NULL)` downstream. Bumping the
cap to 64 caught all 40 in one pass. Crash class GONE.

### Verified PASS

```
[run-test] scenario=load-save-1-show-ingame result=pass
  elapsed_ms=9782 frames=1931 exit=0
```

Filter call sequence (first 16 of many — log cap):
```
call #1  this=10DA39A0 (orphan!) head_slot=10DA46A4 seen=40 filtered=40
call #2  this=1106AA40           head_slot=1106B744  seen=17 filtered=0
call #3  this=10F2E180           head_slot=10F2EE84  seen=14 filtered=0
call #4  this=11000D00           head_slot=11001A04  seen=15 filtered=0
call #5  this=10E0CF60 (engine!) head_slot=10E0DC64  seen=40 filtered=0
call #6-16: various subsystems   head_slot=*           seen=0  filtered=0
```

Interpretation:
- **Call #1** = `sub_5AD9B0(orphan)` — walks orphan's own list. All 40
  entries are self-referential subscribers (wrapper[1] == orphan). All
  filtered. Trampoline sees empty list, returns immediately.
- **Call #5** = `sub_5AD9B0(engine_wilbur)` — engine_wilbur's own list.
  Also 40 entries. NONE filtered (they all point at engine_wilbur, not
  orphan). Trampoline ticks them normally.
- **Calls #2, #3, #4** = other subsystems (e.g. transform mgr,
  render-LOD). None of their lists contain orphan-referencing entries
  in this run — likely because the orphan wasn't fully integrated
  into those subsystems (it only got entity-list registration, not
  full per-subsystem wiring).

So the orphan ends up only in its OWN per-wilbur list (the
self-referential subscribers added by `entity_factory_construct`).
Filtering that list neutralizes all the per-wilbur cascade.

### Cmdline usage

```
pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy `
    -ExtraArgs '-mtrasi-coop-keep-orphan -mtrasi-coop-filter-list-walker'
```

Both flags need to be set:
- `-mtrasi-coop-keep-orphan` enables the b7.2 attach + skips teardown.
- `-mtrasi-coop-filter-list-walker` enables the b7.8 filter.

Without filter, keep-orphan crashes as documented in b7.5/b7.6/b7.7.
Without keep-orphan, filter is inert (`live_orphan_entity()` returns 0).
Off by default; non-coop runs are unaffected.

### What this unlocks

For the first time, the orphan persists through the full test scenario
(~9.8s, 1931 frames) without crashing. Subsystem ticks for the orphan
are SKIPPED (= no aim updates, no weapon updates, no LOD/render, no
ControlMapper-driven movement). The orphan exists as a "frozen" entity
in memory — present in entity_manager's records but excluded from
per-wilbur subsystem processing.

This is the **keep-alive baseline** Phase 2 needed. With this, we can:
- Build P2 input routing without worrying about engine crashes.
- Selectively re-enable orphan ticks by populating only the per-wilbur
  state needed for input-driven movement, while keeping the rest filtered.
- Eventually call the engine's per-wilbur init factory (sub_51F4D0) for
  a fully-ticked P2 — but only after we understand which subsystems
  matter for coop and which don't.

### Permanent additions (b7.8)

- `coop_orphan_filter.cpp` + `mtr/coop_orphan_filter.h` (new module).
- `mtr::coop_spawn_probe::live_orphan_entity()` public getter.
- `-mtrasi-coop-filter-list-walker` cmdline gate. Off by default.
- All earlier diagnostic infra (b7.5/b7.6) intact.

### Trace artifact

Full PASS trace: `research/findings/coop-phase0-b78-filter-pass-trace-2026-05-11.log` (188 KB).

### Build artifact (b7.8)

`mtr-asi.asi` = **712,192 bytes** (+1536 B from 710,656). Includes:
- coop_orphan_filter module
- live_orphan_entity getter on coop_spawn_probe
- All earlier b7.5/b7.6/b7.7 infrastructure

---

## Phase 2 step (b7.9) — Subscriber inventory comparison: orphan ≡ engine_wilbur

**2026-05-11 (final+15.)** Status: **diagnostic confirmed.** Build = **713,216 B**.

### What we built

Extended `coop_orphan_filter` with:
- **Orphan filtered-node inventory** — on the first call where >0 nodes are
  filtered, dump every unlinked wrapper's `(node, wrapper, vtable)` triple.
  One-shot per session.
- **Engine_wilbur baseline dump** — on the first call where `this ==
  engine_wilbur`, do a read-only walk of the same list structure and dump
  the same triples (no filtering, just read). One-shot per session.

Goal: confirm whether `entity_factory_construct` populated the orphan with
the same set of per-wilbur subscribers as the engine populates for the
real player, OR whether some subscribers are missing.

### Result: IDENTICAL inventory, position-for-position

Both lists have **40 nodes**. The vtables at each index are byte-identical
between the two lists:

| Index | Orphan vtable | Engine vtable | Match |
|------:|:--------------|:--------------|:------|
|  0    | `0x006D11F8` | `0x006D11F8`   | ✓     |
|  1    | `0x006C4D40` | `0x006C4D40`   | ✓ (ViewDriver wrapper — known from vdt_probe) |
|  2    | `0x006AFF98` | `0x006AFF98`   | ✓     |
|  3    | `0x006AFDA0` | `0x006AFDA0`   | ✓     |
|  4    | `0x006B05B8` | `0x006B05B8`   | ✓     |
|  5    | `0x006CE488` | `0x006CE488`   | ✓     |
|  6    | `0x006D0F00` | `0x006D0F00`   | ✓     |
|  7    | `0x006AE540` | `0x006AE540`   | ✓     |
|  8    | `0x006B0128` | `0x006B0128`   | ✓     |
|  9    | `0x006D1408` | `0x006D1408`   | ✓     |
| 10-39 | ...           | ...            | all ✓ |

Wrapper instance ADDRESSES differ (separate heap allocations per wilbur)
but the wrapper CLASS (vtable) and ORDER are identical. Each wrapper
`wilbur` field correctly back-references its owner (orphan vs engine).

### Conclusion

`entity_factory_construct` fully populated the orphan's per-wilbur
infrastructure — same 40 subscriber wrappers, same classes, same order
as the real engine_wilbur. **The infrastructure is NOT what's missing.**

What IS missing on the orphan, then? The PER-SUBSYSTEM STATE that each
subscriber wrapper dereferences during its tick:
- The ControlMapper instance in `orphan+0xCCC` registry (we wire this in b7.6).
- The WeaponInventory instance (key string at 0x006A88E8) — NOT wired.
- The aimTurning instance (key string at 0x006C4F40) — NOT wired.
- Various other per-wilbur components that the 38 other subscribers read.

Each subscriber, when ticked, reads its corresponding instance via
`sub_5CB310(wilbur+0xCCC, "<key>", 0)` then dereferences the result.
The orphan's registry has only ControlMapper wired; ALL OTHER subscribers
get NULL back from the registry lookup and crash on their first deref.

### Implications for re-enablement

The structural answer for "make orphan visibly tick" is:
1. For each subscriber we want to re-enable, identify the registry key
   it looks up (the string it passes to `sub_5CB310`).
2. Find/run the engine's per-wilbur init code that normally populates
   that registry entry.
3. EITHER manually replicate the engine's allocation/insert pattern OR
   call the engine factory at `sub_51F4D0` (rr01-thunked) to do all 40
   at once.

For 40 subscribers, manual replication is significant work. The
preferred path is option (3) — find a callable entry into `sub_51F4D0`
or its equivalent. That's deferred to a future session.

### Vtable identification status

Most of the 40 vtable addresses fall into two clusters:
- **0x006AB-0x006B0** (~10 entries) — likely a tightly-related subsystem
  family.
- **0x006C-0x006D** (~25 entries) — another related family.

Some known:
- `0x006C4D40` = ViewDriver subscriber wrapper (confirmed via
  viewdriver_tick_probe's `this+0` reads).

The other 39 vtables are anonymous (their vt[0] entries mostly point
into rr01-thunked code, so IDA's static analysis can't name them).
Identifying each by reading the registry-key string passed to
`sub_5CB310` from inside its `vtable[13]` tick would require either
runtime decoding or per-subscriber probes — both possible but heavy.

### Trace artifact

Full inventory trace: `research/findings/coop-phase0-b78-inventory-trace-2026-05-11.log` (197 KB).

### Build artifact (b7.9)

`mtr-asi.asi` = **713,216 bytes** (+1024 B from 712,192). Includes:
- coop_orphan_filter inventory logging
- engine_wilbur baseline dump helper
- All earlier infrastructure intact
- Same `result=pass elapsed_ms=9625 frames=1930` outcome — diagnostic
  additions did not regress keep-alive.



## Phase 2 step (b7.10) — Smart filter: 21/40 orphan subscribers now tick (no-op)

**2026-05-12 (final+16).** Status: **shipped + PASS.** Build = **714,752 B**.

### Why

The (b7.8) filter unlinks ALL 40 orphan-owned wrappers from the
wilbur+0xD04 list before the trampoline walks it. That works (keep-alive
PASS) but is more aggressive than necessary. A static audit of the 40
subscriber wrapper classes (see "40-class inventory" table below) shows
that **21 of 40 wrappers have `vt[13] == 0x0059AAF0`** — a one-instruction
`ret` stub the engine uses as a placeholder. Their tick is literally a
no-op; running it cannot crash because it reads nothing.

These 21 wrappers exist in the wilbur+0xD04 list to keep the wrapper
linked from other code paths (event dispatch / global-tick path / etc.),
not because their per-wilbur tick does any work. So unlinking them was
needless work and a needless deviation from the engine's normal list
shape.

### What changed

`coop_orphan_filter.cpp`:
- New `kNoopTickStubVA = 0x0059AAF0`, `kVtableTickSlotOffset = 0x34`.
- New `smart_filter_enabled()` cmdline gate
  (`-mtrasi-coop-filter-smart`).
- New `wrapper_tick_is_engine_noop_stub_seh(vtable)` helper — SEH-guarded
  read of `vtable + 0x34` and compare to the stub VA.
- `unlink_orphan_nodes_seh` takes a new `smart_skip` bool. When set, an
  orphan-owned node whose wrapper's `vt[13]` is the no-op stub is left
  linked (the trampoline calls `ret` on it — harmless).
- `WalkStats.skipped_safe` counts the new path; `Stats.nodes_skipped_safe`
  aggregates across calls.
- Per-call log line now includes `skipped_safe=N smart=0/1`.

`coop_orphan_filter.h`: added `Stats.nodes_skipped_safe`.

### Live-test result

```
pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy \
    -ExtraArgs '-mtrasi-coop-keep-orphan -mtrasi-coop-filter-list-walker -mtrasi-coop-filter-smart'

result=pass elapsed_ms=10172 frames=1872

[coop_orphan_filter] call #1 this=10DE49A0 head_slot=10DE56A4
    seen=40 filtered=19 skipped_safe=21 hit_cap=0 smart=1
    (orphan=0x10DE49A0)
```

`seen=40`, `filtered=19`, `skipped_safe=21` — matches the static-analysis
prediction exactly. 21 orphan wrappers now tick on the orphan (as no-ops)
without regression. The other 19 (whose vt[13] is a real tick body) are
still unlinked to prevent crashes from missing per-subsystem state.

### 40-class inventory (vtable, class name, vt[13], noop?)

| Idx | Vtable     | Class                       | vt[13]      | Noop? |
|----:|:-----------|:----------------------------|:------------|:-----:|
|   0 | 0x006D11F8 | Rider                       | 0x005EBA80  |  no   |
|   1 | 0x006C4D40 | ViewDriver                  | 0x005454B0  |  no   |
|   2 | 0x006AFF98 | TargetSelectDriver          | 0x0059AAF0  | **yes** |
|   3 | 0x006AFDA0 | TargetRelativeDriver        | 0x0059AAF0  | **yes** |
|   4 | 0x006B05B8 | WilburDriver                | 0x00459D40  |  no   |
|   5 | 0x006CE488 | SimpleCollision             | 0x0059AAF0  | **yes** |
|   6 | 0x006D0F00 | GroundFollower              | 0x005E93A0  |  no (read-only wilbur+0x58) |
|   7 | 0x006AE540 | HavocBurrowController       | 0x0059AAF0  | **yes** |
|   8 | 0x006B0128 | TBeamController             | 0x004558A0  |  no   |
|   9 | 0x006D1408 | GroundHeight                | 0x0059AAF0  | **yes** |
|  10 | 0x006B02A0 | TransporterCannonController | 0x004566D0  |  no   |
|  11 | 0x006AF278 | PushPullController          | 0x0044D680  |  no   |
|  12 | 0x006AF8C0 | SlideController             | 0x00450B60  |  no   |
|  13 | 0x006AF708 | SidleController             | 0x0044FDA0  |  no   |
|  14 | 0x006AEA90 | LedgeController             | 0x00448310  |  no   |
|  15 | 0x006AE830 | LadderController            | 0x004462C0  |  no   |
|  16 | 0x006CE9E0 | SimpleGroundController      | 0x0059AAF0  | **yes** |
|  17 | 0x006CE048 | HeadControl                 | 0x0059AAF0  | **yes** |
|  18 | 0x006C5068 | Shadow                      | 0x0059AAF0  | **yes** |
|  19 | 0x006D1AB0 | ActorLighting               | 0x0059AAF0  | **yes** |
|  20 | 0x006ACE60 | ScannerMissionSay           | 0x0059AAF0  | **yes** |
|  21 | 0x006C1008 | OnDemandHelp                | 0x0052D4A0  |  no   |
|  22 | 0x006CE540 | OnDemandHelpSay             | 0x0059AAF0  | **yes** |
|  23 | 0x006CE120 | Say                         | 0x0059AAF0  | **yes** |
|  24 | 0x006D1E20 | AvatarFX                    | 0x0059AAF0  | **yes** |
|  25 | 0x006D2440 | Footfall                    | 0x0059AAF0  | **yes** |
|  26 | 0x006AB5B0 | EgyptTutorial               | 0x0059AAF0  | **yes** |
|  27 | 0x006C1920 | DamageSurfaceTracker        | 0x00532860  |  no   |
|  28 | 0x006C1AB0 | VibrateJoystick             | 0x00532B40  |  no   |
|  29 | 0x006CEDD8 | HealthTracker               | 0x0059AAF0  | **yes** |
|  30 | 0x006CF118 | Teleport                    | 0x0059AAF0  | **yes** |
|  31 | 0x006C4F90 | Respawn                     | 0x00546100  |  no   |
|  32 | 0x006CE3E0 | Afflictions                 | 0x0059AAF0  | **yes** |
|  33 | 0x006CEC38 | InteractionMonitor          | 0x005D32D0  |  no   |
|  34 | 0x006D1128 | InventoryList               | 0x0059AAF0  | **yes** |
|  35 | 0x006D1798 | ActionHandler               | 0x0059AAF0  | **yes** |
|  36 | 0x006ADB00 | ClimbController             | 0x0043B310  |  no   |
|  37 | 0x006D19E8 | Magnet                      | 0x0059AAF0  | **yes** |
|  38 | 0x006AE698 | JumpScanner                 | 0x00442BC0  |  no   |
|  39 | 0x006B7BF8 | WeaponInventory             | 0x0048A130  |  no   |

Totals: **21 noop + 19 real = 40**.

### Implications for future selective re-enablement

- **Tier 0 (no state needed, ticked free now)** — the 21 no-op classes.
  Already ticking on orphan after this commit.
- **Tier 1 (read-only-from-wilbur+offset, no registry lookup)** —
  GroundFollower is in this tier: its vt[13] (0x005E93A0) reads three
  floats from `wilbur+0x58` and writes to its own wrapper state, with no
  call into the +0xCCC registry. Likely safe to un-filter individually
  once the wilbur+0x58 state is confirmed initialized on the orphan
  (factory should already do this — needs verification probe).
- **Tier 2 (registry lookup, need per-key mirror)** — Rider, ViewDriver,
  WilburDriver, etc. Each needs the registry keys their tick body
  references mirrored to orphan+0xCCC. ViewDriver alone reads
  `"ControlMapper"`, `"WeaponInventory"`, `"aimTurning"`. The previously
  proposed engine factory `sub_51F4D0` is NOT a callable entry (b7.1-
  next4 confirmed it lands in dead code); per-key replication remains
  the path.

### Cmdline flags (cumulative state, b7.10)

- `-mtrasi-coop-keep-orphan` — keep the spawned orphan alive (skip teardown).
- `-mtrasi-coop-filter-list-walker` — install the (b7.8) sub_5AD9B0 hook.
- `-mtrasi-coop-filter-smart` — **(b7.10)** within the hook, leave orphan
  wrappers linked when `vt[13] == 0x0059AAF0`. Off-by-default opt-in so
  the (b7.8) "filter all" baseline remains the bisect fallback.

### Trace artifact

`research/findings/coop-phase0-b710-smart-filter-trace-2026-05-12.log`
(195 KB, full mtr-asi.log of the (b7.10) PASS run).

### Build artifact (b7.10)

`mtr-asi.asi` = **714,752 bytes** (+1,536 B from 713,216). New code:
- `smart_filter_enabled()` cmdline gate
- `wrapper_tick_is_engine_noop_stub_seh()` helper
- Smart-skip branch in `unlink_orphan_nodes_seh`
- New stat counter wiring
- Updated per-call log line.

---

## Phase 2 step (b7.11) — Audited-safe allowlist: GroundFollower ticks for real

### What changed (one paragraph)

The (b7.10) smart filter only skipped wrappers whose `vt[13]` was the engine
no-op `ret` stub (`0x0059AAF0`). (b7.11) extends the same `-mtrasi-coop-filter-smart`
flag with a hand-audited **vtable allowlist**: orphan-owned wrappers whose
class has been individually verified as safe-to-tick are also left linked,
even though their `vt[13]` is a real function. First entry: **GroundFollower**
(vtable `0x006D0F00`, tick `0x005E93A0`).

**Audit residual (b7.11):** the audit cannot exhaustively prove "nothing
outside the wrapper reads `wrapper+0x28..+0x35`" by static RE alone — that
would require full data-flow tracing of every code path touching a
GroundFollower instance. The argument is structural: nothing else in the
engine ticks the orphan (we've removed it from `sub_5AD9B0`'s visit list
for the 18 unlinked classes; the 21 no-op-stub classes have empty ticks;
the GroundFollower wrapper itself owns the writes), and no other system is
known to consult GroundFollower instances out-of-tick. Live-test post-(b7.11)
PASSed 1871 frames with the data in flight, so any external consumer that
existed would have surfaced by now.

### Why GroundFollower is safe (audit)

Static analysis of `sub_5E93A0` (the GroundFollower tick):

```
5e93a0  push    esi
5e93a1  mov     esi, ecx          ; this = wrapper
5e93a3  mov     eax, [esi]        ; vtable
5e93a5  call    dword ptr [eax+18h]  ; CALL vt[6] (wrapper's own predicate)
5e93a8  test    al, al
5e93aa  jz      short loc_5E93CD   ; predicate false: skip body
5e93ac  mov     ecx, [esi+4]       ; wrapper+4 = wilbur (= orphan in our case)
5e93af  add     ecx, 58h           ; wilbur+0x58
5e93b2  mov     eax, [ecx]
5e93b4  lea     edx, [esi+28h]     ; wrapper+0x28
5e93b7  mov     [edx], eax         ; copy 3 floats
5e93b9  mov     eax, [ecx+4]
5e93bc  mov     [edx+4], eax
5e93bf  mov     ecx, [ecx+8]
5e93c2  xor     al, al
5e93c4  mov     [edx+8], ecx
5e93c7  mov     [esi+34h], al      ; zero wrapper+0x34
5e93ca  mov     [esi+35h], al      ; zero wrapper+0x35
5e93cd  pop     esi
5e93ce  retn
```

The predicate at `vt[6]` (= `0x00422D00` for GroundFollower vtable
`0x006D0F00`) is also entirely benign:

```
422d00  mov     al, [ecx+0Ch]      ; read wrapper+0x0C
422d03  test    al, al
422d05  jz      short loc_422D14    ; if 0, return 0
422d07  mov     al, [ecx+0Eh]       ; read wrapper+0x0E
422d0a  test    al, al
422d0c  jnz     short loc_422D14    ; if non-zero, return 0
422d0e  mov     eax, 1
422d13  retn
```

Net behaviour on the orphan:

- Predicate reads two bytes from the **wrapper's own state** (`wrapper+0xC`,
  `wrapper+0xE`). Wrapper memory is allocated by the same factory as
  engine_wilbur's GroundFollower — by-construction the bytes are bounded
  and readable. Result: predicate cleanly returns 0 or 1.
- If predicate returns 0: function returns immediately. No read of
  `wilbur+0x58`. **Trivially safe.**
- If predicate returns 1: reads 3 dwords from `wilbur+0x58`, `wilbur+0x5C`,
  `wilbur+0x60` (= 12 bytes covering `wilbur+0x58..0x63`, on our **orphan**
  the same range). The orphan is the same shape as engine_wilbur (same
  factory, same construction sequence), so this is well within the
  allocated entity block. Writes those floats to `wrapper+0x28..0x30` and
  zeros `wrapper+0x34/0x35`.
- The wrapper's data at +0x28..0x35 is read by — and only by — other
  GroundFollower methods. Since nothing else in the engine ticks the orphan
  or consults the orphan's GroundFollower wrapper, even garbage floats in
  `orphan+0x58` would be silently consumed inside the wrapper and have no
  gameplay effect.
- Critically: **no `+0xCCC` registry lookup**, **no engine-singleton
  reads**, **no event dispatch**. The audit covers the entire reachable
  state from `vt[13]`.

### Live-test result (b7.11)

```
pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy \
    -ExtraArgs '-mtrasi-coop-keep-orphan -mtrasi-coop-filter-list-walker -mtrasi-coop-filter-smart'
→ scenario=load-save-1-show-ingame result=pass elapsed_ms=9234 frames=1871

[coop_orphan_filter] call #1 this=0x10D7B9A0 head_slot=0x10D7C6A4
    seen=40 filtered=18 skipped_safe=21 skipped_audited=1 hit_cap=0
    smart=1 (orphan=0x10D7B9A0)
```

40 = 18 unlinked + 21 no-op-stub-skipped + 1 audited-skipped. Every
subscriber accounted for. No crash, no slowdown vs (b7.10) (1871 vs 1872
frames over identical wall-clock budget).

Trace artifact:
`research/findings/coop-phase0-b711-groundfollower-trace-2026-05-12.log`
(195 KB, full mtr-asi.log of the (b7.11) PASS run).

### Code shape

`coop_orphan_filter.cpp` additions:
- `struct AuditedVtable { uint32_t vtable_va; const char* class_name;
  const char* audit_note; }` — per-entry record.
- `constexpr AuditedVtable kAuditedSafeVtables[] = { ... GroundFollower ... }`
  — extend this array to add classes after they've been similarly audited.
- `lookup_audited_vtable(vtable)` — linear scan, called only on
  orphan-owned nodes under smart-skip.
- New stat field `nodes_skipped_audited` + `WalkStats::skipped_audited`.
- Per-call log line now: `seen=N filtered=N skipped_safe=N skipped_audited=N
  hit_cap=N smart=N`.

`coop_orphan_filter.h`: added `Stats.nodes_skipped_audited`.

### Build artifact (b7.11)

`mtr-asi.asi` = **714,752 bytes** (unchanged from b7.10 at section
granularity — code delta fits inside existing alignment slack).

### Next-tier candidates (post-b7.11)

The remaining 18 unlinked wrappers are the real work for the per-key
registry mirror path. Roughly grouped by what their tick reads:

**Tier-1 candidates (other read-only-from-wilbur ticks)** — pending audit
each. Inspect their `vt[13]` body and confirm they fit the GroundFollower
pattern (no `+0xCCC` lookup, no singleton read). If yes, just add an entry
to `kAuditedSafeVtables`.

**Tier-2 (per-key registry mirror)** — wrappers whose tick calls into
`+0xCCC`. Generalise the existing `attach_engine_cm_to_orphan` to
`mirror_engine_registry_key(orphan, key_name)`. Apply to "WeaponInventory"
+ "aimTurning" so ViewDriver (`0x006C4D40`) can tick.

**Tier-3 (full per-wilbur registry replication)** — enumerate
engine_wilbur's full `+0xCCC` registry vector and bulk-mirror onto the
orphan. Cleanest end state.

### Audit results for the remaining 18 real-tick classes (b7.11-audit)

After shipping GroundFollower, I batch-disassembled `vt[13]` for every
other real-tick class to find more cheap Tier-1 wins. **None pass strict
Tier-1.** All 18 ticks reach beyond `wilbur+<small fixed offset>` in some
way. Per-class first-pass notes (each based on the first ~30 instructions
of the tick):

| Class                       | vt[13]      | Disqualifier (Tier-1) | Tier |
|-----------------------------|-------------|------------------------|------|
| Rider                       | 0x005EBA80  | calls `sub_55D8F0` (unknown helper) + reads `dword_6CBD8C` (global) | 1.5? |
| ViewDriver                  | 0x005454B0  | SecuROM-thunked + known registry deps (WeaponInventory/aimTurning) | 2    |
| WilburDriver                | 0x00459D40  | reads `wilbur+0xCCC` directly                                       | 2    |
| TBeamController             | 0x004558A0  | reads globals `g_engine_universal_dt_003`, `dword_72988C` + wilbur->vt[17] | 1.5 |
| TransporterCannonController | 0x004566D0  | globals + wilbur->vt[17] + `[wilbur+0x1D4]`                          | 1.5  |
| PushPullController          | 0x0044D680  | globals + wilbur->vt[17] + `sub_5AD820(wilbur, "<key>")`             | 2    |
| SlideController             | 0x00450B60  | calls `sub_450830` (state-machine helper, content unknown)          | ?    |
| SidleController             | 0x0044FDA0  | globals + wilbur->vt[17] + name-registry lookup                      | 2    |
| LedgeController             | 0x00448310  | huge frame (0x248) + SEH + globals + wilbur->vt[17]                  | 2    |
| LadderController            | 0x004462C0  | globals + wilbur->vt[17] + name-registry lookup                      | 2    |
| OnDemandHelp                | 0x0052D4A0  | reads `wilbur+0xCCC` + global `word_728FA4`                         | 2    |
| DamageSurfaceTracker        | 0x00532860  | calls `sub_5320E0` (unknown) + reads `g_engine_universal_dt_003`    | 1.5? |
| VibrateJoystick             | 0x00532B40  | huge function + likely touches global input subsystem               | 2/?  |
| Respawn                     | 0x00546100  | SecuROM stolen-byte thunk - can't statically audit                  | ?    |
| InteractionMonitor          | 0x005D32D0  | writes through `dword_7283D8` global indirection                    | 2    |
| ClimbController             | 0x0043B310  | reads `wilbur+0xCCC` + `sub_5CB310`/`sub_5CB120` (registry walker)  | 2    |
| JumpScanner                 | 0x00442BC0  | first 30 insns are bulk copy from `wilbur+0x58..0x90` (good!) then hits `wilbur+0xCCC` + name lookups | 2 |
| WeaponInventory             | 0x0048A130  | caches 4 registry lookups on first call (`sub_5B04E0`)              | 2    |

The two distinct disqualifier patterns are worth naming:

**Pattern A - "frame-coherent pose cache":** present in most movement
controllers (TBeam, Transporter, PushPull, Sidle, Ledge, Ladder, etc.).
Reads/increments `dword_729898` + `dword_72988C` then calls
`wilbur->vt[17]` with the counter. The vt[17] return is then dereferenced.
This is a per-frame anim/pose cache. **If that cache works on the orphan**,
all of these become "Tier 1.5" (just-need-globals-safe). The audit of
wilbur's vt[17] body would tell us. If it depends on registry state,
they're all Tier 2.

**Pattern B - "named registry key lookup":** `sub_5AD820(wilbur, str)` or
`lea ecx, [wilbur+0xCCC] / sub_5CB310(...)`. This is the per-key registry
read. Always Tier 2 - must mirror the key onto the orphan first.

### Recommendation

**Tier 1 is exhausted at GroundFollower (b7.11).** Net gain: 1/19.
Diminishing returns on continued static audit for cheap wins.

Two paths forward, in order of effort:

1. **Tier 1.5 expansion** - Statically audit
   - `wilbur->vt[17]` (= what `[wilbur_vtable+0x44]` does)
   - `dword_729898` / `dword_72988C` (engine globals - read pattern looks
     like frame counters, very likely safe but not proven)
   - The handful of unknown helpers: `sub_55D8F0`, `sub_450830`, `sub_5320E0`
   - If wilbur's vt[17] is read-only from `wilbur+<fixed offsets>` (likely:
     it's a per-frame anim pose getter), unlock 5+ controllers (TBeam,
     Transporter, PushPull, Sidle, Ledge, Ladder, Rider).

2. **Tier 2 (per-key registry mirror)** - Generalise
   `attach_engine_cm_to_orphan(orphan, "ControlMapper")` to
   `mirror_engine_registry_key(orphan, key_name)`. Apply to "WeaponInventory"
   (strings at `0x6B7E60`/`0x6A98CC`/`0x6A713C`/`0x6B7E50` per WeaponInventory
   tick), "aimTurning", "ControlMapper". Then un-filter ViewDriver and
   WeaponInventory in one shot. Unlocks ~7 controllers.

The Tier-1.5 path is mechanically cheap and high-impact if `wilbur->vt[17]`
is safe. Recommended next step.




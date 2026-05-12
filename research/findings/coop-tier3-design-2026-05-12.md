# Tier-3 design: full +0xCCC registry replication

**Date:** 2026-05-12 (after 2-agent strategy audit pivot).
**Goal:** Replace `coop_orphan_filter`'s symptom-suppression with proper
init: clone engine_wilbur's `+0xCCC` registry onto the orphan so EVERY
subscriber ticks safely. Per RULE №1: no crutches, do it properly.

---

## What I learned from the registry RE

Decompiled `sub_5CB310` (lookup), `sub_5CB420` (insert), and `sub_5D3730`
(hash-table search). The registry at `wilbur+0xCCC` is a composite:

```
wilbur + 0xCCC      ──────► [registry struct]
                              ├─ +0x00  vec_handle*    ────► [vector] holds slot pointers
                              │                                ├─ +0x00 data* (slot[])
                              │                                ├─ +0x04 size
                              │                                ├─ +0x08 capacity
                              │                                └─ +0x0C ...
                              │
                              └─ +0x08  [hash table struct]
                                   ├─ +0x04 (this[1]) top-bucket shift
                                   ├─ +0x08 (this[2]) bottom-bucket mask
                                   ├─ +0x10 (this[4]) top-bucket array*
                                   ├─ +0x1C (this[7]) used-bucket count
                                   ├─ +0x24 (this[9]) resize threshold
                                   └─ +0x28 (this[10]) capacity mask
```

Two-level bucket array: `top_array[hash>>shift][hash & bot_mask]` =
linked-list head. Each node is `(next, hash, key_ptr, key_len, ...payload)`.
Payload starts at node+0x10 and IS the slot record.

**Slot record (28 bytes):**

| Offset | Field          | Notes |
|--------|----------------|-------|
| +0x00  | owner backref  | usually the wilbur entity address |
| +0x04  | byte flag      | set to 1 when storage cell allocated |
| +0x08  | flag2          | (a6 in sub_5CB420) |
| +0x0C  | storage cell*  | points at a small allocation (size = type-dependent) |
| +0x10  | type           | 0/1/5 = 4-byte cell, 2 = 12, 3 = 1, 4 = 36, 6 = 4 (zeroed) |
| +0x14  | name hash      | from `sub_5D3E30(name)` |
| +0x18  | byte           | "storage owned by slot" flag |

The instance pointer is `*(slot+0x0C)` = `*storage_cell`. Type 5 is
"resource"; types 0/1/6 are also single-dword cells.

---

## Tier-3 plan

### Phase 1: enumerate

Walk engine_wilbur's hash table, collecting `(key_ptr, key_len, slot*)` for
every entry. Already proven: SEH-safe, no allocations.

Implementation: `enumerate_engine_registry(engine_wilbur, visitor)`:
1. `registry = engine_wilbur + 0xCCC`
2. `vector_handle = *(registry)`; iterate the slot vector at `*(vector_handle)`
   for size = `*(vector_handle + 4)` slots. Fast, ordered.
3. For each slot, recover its name by walking the hash table at
   `registry + 8` for the matching hash (slot+0x14) — O(1) avg via
   2-level bucket lookup; we already have the hash. Cheaper: walk hash
   table directly (it's the source of truth) and the vector is redundant.

Trade-off: vector-first is simpler (just `data[i]` for i in 0..size), but
recovering the name requires a second lookup. Hash-table-first gives names
directly but the walker is more complex.

**Choice: vector-first.** Build a `slot* → name` reverse map by walking the
hash table ONCE up front; thereafter iterate the vector.

### Phase 2: mirror

For each `(name, slot)` from the engine registry, call the existing
`sub_5CB420(orphan_registry, 0, orphan_addr, name, slot.type, 0, 0)` to
allocate a fresh slot + storage cell on the orphan. Then
`sub_5CB220(orphan_slot, 0, engine_instance)` to wire the engine's instance
into the orphan's cell. This is the (b7.6) pattern, iterated.

**Sharing model (Tier-3a):** orphan's slot record is fresh (own owner
backref, own hash entry), but the *instance* is shared with engine. This
mirrors the (b7.6) ControlMapper precedent: routing per-player happens via
the `b2-rem-2` component thunks, not via separate instances. Most slots
will be safe to share; the network layer (Phase 1) overrides where
per-player state is needed.

### Phase 3: install + flag

New cmdline `-mtrasi-coop-mirror-registry`. When set, after
`entity_factory_construct` returns the orphan, call
`mirror_engine_registry_to_orphan(orphan)` BEFORE the engine's first
sub_5AD9B0 tick.

When mirror is on:
- `coop_orphan_filter` is **bypassed** (set its `filter_enabled()` to
  return false if mirror is on, or wire the gating in the install path).
- No subscriber tick should crash, because every key it consults resolves.
- All 40 subscribers tick, no unlink/relink, no smart filter, no allowlist.

If mirror is off (default): existing behaviour unchanged.

### Phase 4: verify

Post-mirror, log: `orphan_registry.size = N` and compare to engine's. Walk
orphan registry, confirm each key resolves. Live-test:
```
pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy \
    -ExtraArgs '-mtrasi-coop-keep-orphan -mtrasi-coop-mirror-registry'
```
Expected: `result=pass`, no `coop_orphan_filter` activity in log (or filter
disabled), no crash from any subscriber tick.

### Phase 5: retire

If Phase 4 PASSes consistently:
1. Mark `coop_orphan_filter.cpp` as deprecated in the file header.
2. Plan its removal for the session AFTER Phase 4 ships green twice.
3. Update Phase 1 plan to use `mirror_engine_registry_to_orphan` as the
   host-side P2 init step.

---

## Risk analysis

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Hash table walk faults on uninitialized buckets | Medium | SEH wrap each level; abort gracefully |
| Some types need post-insert init beyond write | Medium | Match what b7.6 does (single write); if subscribers still crash, audit per type and add per-type init |
| Sharing instances creates per-player state bleed | High (long-term) | Phase 1 networking layer fixes via b2-rem-2 thunks; Tier-3 only requires no-crash, not no-bleed |
| Some registry keys are populated AFTER factory return (lazy init) | Medium | Run mirror multiple times if needed; or hook the lazy-init function and mirror on demand |
| The vector + hash table can be out of sync mid-mutation | Low | We only read engine's registry, never write; readers are SEH-guarded |
| sub_5CB420 has additional side effects I haven't audited | Low | Already proven safe by b7.6 (called for ControlMapper); same call for other types |

---

## Estimated scope

- Phase 1 (enumerate): ~80 LOC + 1 small test/probe to dump engine registry.
- Phase 2 (mirror): ~60 LOC, reuses existing `attach_engine_cm` primitives.
- Phase 3 (install + flag): ~30 LOC, mostly wiring.
- Phase 4 (verify): live-test + log inspection.
- Phase 5 (retire filter): 1-line gate + later deletion.

Total: ~170 LOC + verification. Probably 1 long session or 2 shorter ones.

---

## Open questions for the maintainer

1. **Sharing vs cloning instances**: Tier-3a (shared, like b7.6) is the
   plan above. Tier-3b (cloned per-player) is more work but more correct
   long-term. Plan goes with Tier-3a as MVP; Tier-3b can be a per-key
   refinement once Phase 1 networking exposes which keys actually need
   per-player state.

2. **Probe-first or implement-direct**: Should I write a registry-DUMP
   probe FIRST (no mutation, just logs the 40 keys + types) and ship that
   independently, OR jump straight to enumerate+mirror? The probe alone
   validates Phase 1 enumerate logic.

3. **Filter deprecation timing**: bypass filter immediately when mirror is
   on (Phase 3), or run both for a session as belt-and-suspenders?

---

## (b7.12) DUMP PROBE — SHIPPED + GREEN (2026-05-12)

Implemented in `src/mtr-asi/src/coop/coop_registry_mirror.cpp` + header.
Cmdline gate `-mtrasi-coop-dump-registry`. Wired into the keep-orphan path
right after `attach_engine_cm_to_orphan`. Live test:

```
result=pass elapsed_ms=9719 frames=1931
[coop_registry_mirror] dump end: vector_size=21 slots_dumped=21
                                  names_resolved=21 (hash_entries_collected=21)
```

### Key findings from the dump

**1. The +0xCCC registry has 21 keys, NOT 40.** The "40" we kept seeing was
the per-wilbur subscriber list at `+0xD04` (wrapper instances), not the
per-wilbur data registry at `+0xCCC`. These are separate structures with
separate purposes. One of the dumped keys, `maxComponents`, has value
`0x28 = 40` — that's where the 40 was always coming from.

**2. The complete 21-key inventory** (engine_wilbur on a real save load):

| # | Name | Type | *storage (sample value) |
|---|------|------|------------------------|
| 0 | gravity | 1 (float) | 0xC1B00000 (= -22.0) |
| 1 | DesiredDriverSpeedSquared | 1 (float) | 0.0 |
| 2 | onGround | 3 (byte) | 0x01 |
| 3 | onSteepGround | 3 (byte) | 0x00 |
| 4 | onActor | 6 (dword zeroed) | 0x00 |
| 5 | groundNormal | 2 (vec3) | (3-dword storage) |
| 6 | groundPosition | 2 (vec3) | (3-dword storage) |
| 7 | stopPushing | 0 (dword) | 0x00 |
| 8 | terrainCollide | 0 (dword) | 0x00 |
| 9 | fellToDeath | 3 (byte) | 0x00 |
| 10 | health | 1 (float) | 0x42F00000 (= 120.0) |
| 11 | groundScanPoints | 0 (dword) | 0x03 |
| 12 | bControlDisabled | 3 (byte) | 0x00 |
| 13 | maxComponents | 0 (dword) | 0x28 (= 40) |
| 14 | capsuleNodePtr | 5 (resource) | pointer-to-instance |
| 15 | capsuleOrigin | 2 (vec3) | 0x00 |
| 16 | capsuleEnd | 2 (vec3) | 0x00 |
| 17 | capsuleRadius | 1 (float) | 0x3E99999A (= 0.3) |
| 18 | focusOffset | 2 (vec3) | 0x00 |
| 19 | AvatarFriendly | 3 (byte) | (uninit-looking) |
| 20 | ControlMapper | 5 (resource) | pointer-to-CM-instance |

**3. Type distribution:**
- type 0 (5 keys): single dword
- type 1 (4 keys): single float
- type 2 (5 keys): vec3 (12 bytes)
- type 3 (5 keys): single byte
- type 5 (2 keys): resource pointer (single dword pointer)
- type 6 (1 key): single dword zeroed

**4. `sub_5AD820` is NOT a `+0xCCC` registry call.** Disasm:
```
sub_5AD820:
    add  ecx, 0D04h
    jmp  sub_5D38D0
```

It operates on `wilbur+0xD04` (the subscriber list), so when wrapper
ticks call `sub_5AD820(wilbur, "<some-name>")` they're looking up SIBLING
subscriber wrappers by name, not registry keys. The orphan's subscriber
list IS populated by `entity_factory_construct`, so these lookups should
succeed on the orphan post-spawn.

This means the only +0xCCC-dependent crashes are the ones where wrappers
explicitly do `lea ecx, [wilbur+0xCCC]; call sub_5CB310/sub_5CB350` (e.g.
WilburDriver, ClimbController, OnDemandHelp). Mirroring the 21 keys
should resolve those.

### What (b7.13) needs

For each of the 21 keys:
1. Insert into orphan via `sub_5CB420(orphan+0xCCC, 0, orphan_addr, name, type, 0, 0)` — allocates a fresh slot AND a fresh storage cell sized for `type`.
2. Copy the engine's storage **VALUE** into the orphan's new storage cell.
   - For types 0/1/3/5/6 (4-byte or 1-byte cell): `sub_5CB220(orphan_slot, 0, engine_value)` works.
   - For type 2 (vec3, 12 bytes): need a multi-dword write — likely a direct memcpy into `*(orphan_slot+0x0C)`.
   - For type 4 (36 bytes): not seen in our 21 keys — defer until needed.
3. For type 5 (resource): the engine's "value" is itself a pointer to an instance. Sharing the instance is the (b7.6) precedent — write the instance pointer into orphan's cell. Same code path as a regular dword write.

### Estimated b7.13 implementation

- `mirror_engine_registry_to_orphan(orphan)` function: ~80 LOC.
- Per-type writer helper (handles type 2 vec3 separately): ~30 LOC.
- New cmdline flag `-mtrasi-coop-mirror-registry`.
- Wired in spawn-probe keep-orphan path same place as the dump.
- Live test with NO filter flags (only `-mtrasi-coop-keep-orphan -mtrasi-coop-mirror-registry`).
- Expect: no crash. If a wrapper still crashes, it's a non-registry-dependent issue (likely a +0xD04 wrapper's own state).

Trace artifact:
`research/findings/coop-phase0-b712-registry-dump-trace-2026-05-12.log` (195 KB).

---

## (b7.13) MIRROR MUTATOR — SHIPPED + GREEN (belt-and-suspenders) (2026-05-12)

Build = **720,384 B**. Repro (with filter still installed as backstop):

```
pwsh tools/run-test.ps1 -Scenario load-save-1-show-ingame -Redeploy \
    -ExtraArgs '-mtrasi-coop-keep-orphan -mtrasi-coop-filter-list-walker
                -mtrasi-coop-filter-smart -mtrasi-coop-mirror-registry
                -mtrasi-coop-dump-registry'
→ result=pass elapsed_ms=9797 frames=1931
→ [coop_registry_mirror] mirror end: seen=21 attempted=21 inserted=21
                                       copied=21 unknown_type=0 faults=0
→ B7.13 post-mirror ORPHAN dump: vec_size=21 dumped=21 names=21 faults=0
```

### What landed

- `mirror_engine_registry_to_orphan(engine_wilbur, orphan)` in
  `coop_registry_mirror.cpp`. Iterates engine_wilbur's 21-key registry,
  for each key:
  1. Looks up the key name in the side-table built from the hash-table walk.
  2. Calls `sub_5CB420(orphan+0xCCC, 0, orphan, name, type, 0, 0)` — engine's
     own insert. flag1=0 makes it allocate a fresh storage cell sized to type.
  3. Calls `sub_5CB310(orphan+0xCCC, 0, name, 0)` to retrieve the actual
     slot pointer (the insert's return is NOT the slot — pattern stolen from
     b7.6 `attach_engine_cm_to_orphan`).
  4. Direct SEH-guarded memcpy from `engine_storage` to `orphan_storage`,
     `size_bytes = cell_size_for_type(type)`.
- Per-type cell sizes: 0→4, 1→4, 2→12, 3→1, 4→36 (unused), 5→4, 6→4.
- Cmdline gate: `-mtrasi-coop-mirror-registry`. New `mirror_enabled()` helper.
- Post-mirror orphan-side dump (chained off `-mtrasi-coop-dump-registry`)
  confirms orphan now has the same 21 keys with matching types and values
  (resource-type keys 14/`capsuleNodePtr` and 20/`ControlMapper` share the
  engine's instance — same precedent as b7.6 ControlMapper).

### Initial bug + fix (caught by belt-and-suspenders test)

First attempt at the mutator interpreted `fn_insert`'s return value as the
slot pointer. **Wrong** — per b7.6's existing code, `fn_insert`'s return
is NOT the slot; you must call `fn_lookup` AGAIN with the same name. The
buggy version returned arbitrary garbage pointers (same address aliased
across multiple inserts), and the subsequent memcpy wrote to those garbage
addresses, **corrupting the orphan's +0xD04 subscriber list**. This
triggered N×VEH faults in `coop_orphan_filter` (the filter walking a
corrupted list).

Fix: do `fn_insert(...)` then `fn_lookup(...)` for each key.

The belt-and-suspenders test config (filter + mirror) was crucial here —
the filter's VEH faults were the visible signal that mirror was writing
where it shouldn't. Without the filter in place, the corruption would have
manifested as a hard crash with much harder diagnosis.

### Verification

| Test config | Result | Notes |
|-------------|--------|-------|
| Filter ON, Mirror ON | **PASS 1931 frames** | Mirror inserted 21/21, copied 21/21, 0 faults. Filter still filters 18 + skips_safe 21 + skips_audited 1 — but mirror's correctness is independent. |
| Filter ON, Mirror ON, orphan-side dump | **21 keys present** on orphan | health=120.0, capsuleRadius=0.3, ControlMapper shared with engine, etc. |
| Filter OFF, Mirror ON | **CRASH** at `sub_58D330(NULL)` | Already-known LOD/render-path issue (memory: `sub_4CD7B0 → sub_58D330(NULL)`). Distinct from +0xCCC. Not in b7.13 scope. |

### Why filter retirement is BLOCKED (one more thing needed)

Mirror addresses the +0xCCC-registry-consumer crashes (WilburDriver,
ClimbController, OnDemandHelp, and any other wrapper directly calling
`sub_5CB310(wilbur+0xCCC, key, 0)`). It does NOT address the second-class
crash at `sub_4CD7B0 → sub_58D330(NULL)` — that's a NULL passed to
sub_58D330 from somewhere on the LOD/render side. The filter currently
masks both classes; retiring the filter requires also fixing the
sub_58D330 NULL caller.

`sub_58D330` body: `push esi; mov esi, [ecx+0x50]; test esi; jz ...` —
dereferences `this+0x50`. Caller `sub_4CD7B0` is on the render-tick path.
Next session's first task: identify what passes NULL to sub_58D330 on the
orphan, and either populate the missing data or hook the call site.

### Sharing model decision

All 21 keys mirrored as **Tier-3a (shared instance for resource types,
snapshot for value types)**. For type 5 (resource), the orphan's storage
cell holds a copy of the engine's instance pointer — both wilburs see the
same CM/capsuleNodePtr instance. Phase-1 networking is expected to override
per-player state via the b2-rem-2 component thunks for the keys that need
per-player isolation. No need for Tier-3b (separate instances) until that
audit surfaces.

### Cmdline flag inventory after b7.13

- `-mtrasi-coop-keep-orphan` — orphan survives factory.
- `-mtrasi-coop-filter-list-walker` — install sub_5AD9B0 unlink hook (BACKSTOP — scheduled-for-deletion after sub_58D330 fix).
- `-mtrasi-coop-filter-smart` — within filter: skip no-op-stub + audited-safe wrappers.
- `-mtrasi-coop-dump-registry` — read-only dump engine_wilbur's +0xCCC (b7.12).
- `-mtrasi-coop-mirror-registry` — **(b7.13 NEW)** actually clone the 21 keys to the orphan + post-mirror orphan dump if dump flag is also set.

### Trace artifact

`research/findings/coop-phase0-b713-mirror-trace-2026-05-12.log` (195 KB) —
clean GREEN run with filter+mirror+dump enabled. Shows full mirror trace
+ post-mirror orphan dump matching engine's 21 keys.

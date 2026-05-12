# Coop Phase 0 — replication primitive found in binary (2026-05-11)

## Status: MAJOR FINDING — Phase 3 (replication) likely much smaller than v2 plan estimated.

The engine ships with a **fully-coded "DistributedState" replication mechanism**: paired publish/receive primitives, a transform-replication side-channel for sub-actors, and 10 entity classes already wired to participate — including `Protagonist` (the player class). The primitives are no-ops in single-player because the network manager pointer (`entity+216`) is null, but the code path is live: implement the network-manager interface and the existing publish/receive calls do real work.

This investigation was the "fork B" recommendation from the Phase 0C catalog checkpoint: verify the vestigial MP scaffolding via IDA RE before re-costing Phase 1+ estimates.

## Primitives (renamed in IDB)

| Old | New | Purpose |
|---|---|---|
| `sub_5AFDB0` | `entity_publish_distributed_state` | Publish 18-dword bulk state |
| `sub_5AFE90` | `entity_receive_distributed_state` | Receive 18-dword bulk state |
| `sub_5B06A0` | `entity_publish_netactor_transforms` | Publish transform to 4 sub-actors |
| `sub_5B2080` | `entity_reset_and_publish` | Template-method called from 10 class vtables |

### entity_publish_distributed_state (0x5AFDB0)

```c
char __thiscall entity_publish_distributed_state(int this)
{
    if (*(BYTE*)(this+221))            // dirty flag clear → skip
        return ...;
    void* mgr = *(void**)(this+216);   // network manager pointer
    if (!mgr) return ...;              // null in SP — no-op
    int* buf = mgr->vtable[11]("DistributedState");
    if (!buf) return ...;
    // copy 18 dwords from entity+88..168 into buf
    buf[0..17] = *(DWORD*)(this+88..168);
    mgr->vtable[10]();                 // commit
}
```

The function takes an entity, asks the network manager for a 72-byte buffer keyed by the string "DistributedState", fills it with 18 contiguous dwords from `entity+88..168`, and commits via vtable[10].

### entity_receive_distributed_state (0x5AFE90)

Inverse path: `mgr->vtable[12]("DistributedState")` returns a remote-published buffer, copies 18 dwords back into `entity+88..168`, returns 1 (received) or 0 (no buffer available).

### entity_publish_netactor_transforms (0x5B06A0)

For each of up to 4 sub-actors at `entity+492..504` (`entity[123..126]`):
- Check sub-actor's dirty flag at offset 240 (`subactor[60]` sign bit).
- If dirty, write a 16-dword transform record (pos × 2, orient, velocity, 1.0f) into the sub-actor.
- Final call to `entity->vtable[47]` (per-class post-publish hook).

This is the per-frame transform-replication primitive — matches the **`transremote`** script verb from the Phase 0C catalog.

### entity_reset_and_publish (0x5B2080)

Template-method entry, **same function pointer in 10 different class vtables** at varying slot numbers (slot 12 for `wilbur`/`digdug`, slot 13 for `Protagonist`, etc.). The function:

1. Calls `entity->vtable[24]` (per-class hook A)
2. Calls `entity->vtable[46]` (per-class hook B)
3. Calls `entity_publish_netactor_transforms(entity)`
4. Loops over 4 child pointers and calls `sub_4C4960(*ptr)`
5. Sets `*(entity+248) = 15` (state byte)
6. Calls `entity_publish_distributed_state(entity)`
7. Sends `"ResetCamera"` message to `*(entity+424)` (some camera-target ref)

Net effect: on entity reset/respawn, the 10 participating classes flush their state through the replication primitive.

## 10 participating classes (vtables containing sub_5B2080)

| VTable region | Class name (decoded from preceding ASCII data) | Notes |
|---|---|---|
| 0x6a7444 | **digdug** | Mini-game entity |
| 0x6b862C | **wilbur** | Player avatar lowercase |
| 0x6b87?? | (sub-class — name not in preceding bytes) | Adjacent to wilbur; probable wilbur variant |
| 0x6becd?? | **lobbed** | Lobbed weapon/projectile class |
| 0x6bf3?? | (sub-class) | Probable weapon-base variant |
| 0x6c67?? | **triggerbox** | Trigger volume |
| 0x6c6a?? | (terrain prop) | Adjacent to "Plants"/"Trunk"/"FlowersA" strings |
| 0x6cc9A8 | **Protagonist** | THE PLAYER ENTITY CLASS (from entity factory RE) |
| 0x6dce?? | (unidentified) | — |
| (10th) | (unidentified) | — |

Three of the four readable class names are mini-game or projectile classes; **Protagonist and wilbur are both player classes**. The replication primitive is genuinely wired for the player.

## What this means for the v2 plan

### v2 estimates that need revision

| Phase | v2 estimate | Likely revision | Reason |
|---|---|---|---|
| Phase 1 (transport) | 4 wk | Unchanged — still need to build UDP transport | But: the consumer of the transport is now a known interface (3 vtable methods on a "network manager" object), not a new API we have to design |
| Phase 3 (replication) | 4 wk | **Possibly 1-2 wk** | Most of the work is *implementing the network-manager interface*, not designing replication. The publish/receive sites already exist. |
| Phase 5 (script VM replication) | 8 wk | **Possibly 2-3 wk for the engine plumbing** | Same reasoning: many script-VM verbs (`ActorSetStateDistributed`, `ActorGetNetMaster` etc.) presumably call into the same DistributedState mechanism via different keys. Need to confirm but the engine side is built. |

Potential savings: **~10 wk across Phase 3 and Phase 5** if the interface holds up to deeper RE.

### What is STILL needed in Phase 1

The replication primitives all dereference `entity+216` as a *network manager*. The vtable methods used are:

- `vtable[10]` — commit (offset 40)
- `vtable[11]` — allocate/get-for-write buffer by name (offset 44)
- `vtable[12]` — get-for-read buffer by name (offset 48)

Phase 1 needs to:

1. Build the UDP transport (still ~2 wk of work).
2. **Implement a C++ class with that vtable layout**, backing each named-buffer slot with serialize/deserialize to/from the UDP transport.
3. Install instances on player entities by writing `(player_entity+216) = network_manager_ptr` at the appropriate lifecycle point.

The host's `vtable[11]` writes back to a buffer that the host then serializes and sends; the client's `vtable[12]` returns a buffer the client has just deserialized from the wire. Replication keys (string names like "DistributedState") become channel IDs.

This is a *significantly* easier Phase 1 design problem than "build replication from scratch."

### What this does NOT yet tell us

- Whether `(entity+216)` is *always* expected to be set, or only on networked entities. If null, all replication calls become no-ops, so Phase 1 could potentially run with sparse replication coverage.
- Where `(entity+216)` is set in the entity lifecycle. We need to find a write site to know when to install our network-manager pointer (probably in the entity factory or a post-construct hook).
- How many other vtable methods on the network manager are called by code paths we haven't seen yet (would the player publish other named state buffers? probably yes — needs an audit).

## What is NOT in the binary (negative findings)

- `IsMultiPlayer`, `ActorGetNetMaster`, `MultiPlayer_Internet`, `MultiPlayer_LAN`, `GenericNetActor`, `ActorSetStateDistributed` — all exist as strings but have **zero code xrefs**. These are NOT bound to C++ functions by direct address. They are either:
  - Script-VM-only identifiers (the `.sx` VM does hash-keyed lookup against a table built at link time, or the C++ side parses the .sx file and looks up by name dynamically); OR
  - Vestigial strings from a removed/dead-code C++ MP layer.

  To distinguish, the next step is **hook script-VM symbol resolution at runtime** (`script_register_command` hook or whatever the .sx command-dispatch fn is) and see if any of these names resolve to C++ callbacks during gameplay. Deferred — needs game launch.

- `PathFollowerNetActor`, `transremote`, `recv` — found in `.sx` files (per Phase 0C catalog) but NOT as strings in the binary. They are pure script-side identifiers; if they map to anything, it's via the same hash-resolution mechanism above.

## Next-session candidates (highest-leverage first)

1. **Find writes to `(entity+216)`.** This identifies where the network manager pointer is set on entities. Find the function, find what creates the network manager, find whether it's reachable from the current binary. If reachable, Phase 3 is mostly an "implement the vtable" exercise.
2. **Trace what calls `entity_publish_distributed_state` outside of the reset path.** Probably per-frame from somewhere. That's the data flow we need to keep alive.
3. **Find the network manager class itself.** Look for ctors that produce something with vtable[10/11/12] in that role. Likely lives in a constructor that calls a `new` with that vtable.
4. **Live hook of `.sx` symbol resolution.** Validates whether `IsMultiPlayer` et al. are reachable at runtime. Needs game launch (deferred per session rule).

## Files updated

- `ida/400000.Wilbur.exe.i64` — 4 renames, 4 set_comments.
- `research/findings/coop-phase0-replication-primitive-found-2026-05-11.md` (this file) — full writeup.

No mod build for this phase; it's pure RE.

## Engine VAs

| Address | Symbol |
|---|---|
| 0x5AFDB0 | `entity_publish_distributed_state` |
| 0x5AFE90 | `entity_receive_distributed_state` |
| 0x5B06A0 | `entity_publish_netactor_transforms` |
| 0x5B2080 | `entity_reset_and_publish` |
| 0x6CC9A8 | `Protagonist` vtable start (sub_5B2080 at slot 13) |
| 0x6B862C | `wilbur` vtable start (sub_5B2080 at slot 12) |
| 0x6cbd98 | string "DistributedState" |
| 0x6cd79c | string "ActorSetStateDistributed" |
| 0x6cd7b8 | string "ActorGetNetMaster" (no code xrefs) |
| 0x6c4048 | string "IsMultiPlayer" (no code xrefs) |
| 0x6df17c | string "GenericNetActor" (only ref: a function that just returns the string) |

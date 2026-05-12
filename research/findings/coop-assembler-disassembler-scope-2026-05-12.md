# Coop scope: Assembler / Disassembler progression loop

**Date**: 2026-05-12
**Phase**: 2.x (after Phase 2.0 input replication)
**Status**: Scope decision documented. RE work pending.

## Context

MTR's gameplay progression loop revolves around two engine systems:

1. **Disassembler gun** — a player weapon. Shooting dismantlable
   level objects breaks them into resources. Each level has placed
   dismantle targets that drop different resource types.
2. **Assembler device** — a level-placed kiosk / workbench. Player
   walks up, presses interact, opens a craft UI. Spending resources
   from the player's pool unlocks new weapons, cheats, or gadgets.
   Unlocked items are then permanently available for the rest of the
   run (until save reset).

This is the core progression loop. Without it, P2 in a coop session
never advances and stays at the starting loadout for the entire game.

## Scope decision (2026-05-12, after user feedback)

**Resources and unlocks are shared (host-side, host-authoritative).**

- One resource pool per session, owned by the host's engine state.
- One unlocked-blueprint set per session, owned by the host's engine
  state.
- Either player can contribute to the pool by firing the disassembler.
- Either player can spend from the pool by using an assembler kiosk.
- Unlocked items are usable by both players.

**The assembler UI is per-player on the activating player's screen.**

- When a player presses interact on a kiosk, a craft menu opens on
  THEIR screen only (the player who pressed interact).
- The other player keeps playing — they just see the activating
  player's Wilbur standing next to the kiosk.
- Both players can have their own kiosk menu open simultaneously
  (different kiosks or even the same one — concurrent reads of the
  blueprint catalog are safe).
- When the client confirms a craft, a `coop_craft_blueprint` RPC is
  sent to the host. The host validates that the pool has enough,
  deducts the cost, sets the unlocked bit, and broadcasts the updated
  state. Concurrent craft commits are serialized in arrival order by
  the host.

## Why this is in scope

- The disassembler IS a weapon, and weapons are already in scope (per
  COOP_SCOPE.md). Replicating its fire action is just Phase 2.0 input
  replication — the disassembler fires when ControlMapper sees the
  fire button, exactly like every other weapon.
- The assembler IS a scripted interactable + UI screen. Both
  "interactables" and "UI overlays on each player's screen" are
  already in scope. The new piece is the RPC + the small host-side
  validation logic.
- Splitting resource pools per-player would directly contradict the
  in-scope "shared host inventory" decision and would require new
  per-player state tracking on every world resource drop.

This decision is the minimum-viable extension of existing scope to
cover the actual progression loop the game is built around.

## Why we can't skip it

If we shipped coop with disassembler/assembler unsupported:
- P2 disassembler shots would either (a) do nothing or (b) silently
  drop resources into a void that nothing reads. Player feedback would
  be "my gun does nothing meaningful".
- P2 would never get new weapons — stuck on the starting loadout for
  the entire game. The host (with a save partway through the campaign)
  has many weapons; P2 has just one. Wildly broken UX.
- The host could try to "carry" P2 by crafting everything on the
  host's screen, but the host's UI is the same kiosk UI — interrupting
  the host's flow to manually craft for P2.

Compared to: "we just ship it, ~1 week of work, the progression loop
just works" — there's no scenario where skipping is right.

## RE TODOs (Phase 2.x prerequisites)

### TODO 1 — locate the resource pool

The pool is per-resource-type (multiple resource types in MTR — wood,
metal, etc., possibly others). The data lives somewhere reachable by
both:
- The disassembler-shot credit function ("+N of type T").
- The assembler-UI cost-display + cost-deduct paths.

Candidates:
- `dword_*` globals near other progression state.
- A struct hung off the player entity (`Wilbur` instance), e.g. at
  some offset within `protagonist`'s extended state.
- A struct in the save-file write area (we know save struct layout
  from Phase 0B finding F2).

**RE approach**: open a save file partway through the campaign with
known resource counts. Search the save file bytes for those counts.
Cross-reference where the engine reads/writes those bytes during
boot/save.

### TODO 2 — locate the unlocked-blueprint bitmask

Probably a bitfield or array near the resource pool. The assembler UI
must read it to display "locked" vs "available", and the craft-commit
path must set bits.

**RE approach**: same save-file-diff approach. Save with a known
unlock state, modify in-game (unlock one new blueprint), save again,
diff the bytes.

### TODO 3 — locate the assembler-UI screen entry

This is a screen in the `screen_push` system. We have the screen
infrastructure already RE'd (see project_screen_system memory).

**RE approach**: enumerate `g_screen_list` at boot, find names matching
"assembler" or similar. Once found, the screen has a vtable; the
craft-confirm path is one of its callbacks (probably triggered by the
RETURN-on-blueprint-row action).

### TODO 4 — locate the disassembler-shot resource credit function

When the disassembler gun's beam connects with a dismantlable target,
the engine calls some function that:
- Destroys the target (or marks it dismantled).
- Spawns a "+N resource" floating pickup.
- Updates the resource counter when the pickup is collected (or
  immediately, depending on game design).

**RE approach**: pick a known disassemble-able object in a level, set
a write-watchpoint on the resource pool address (from TODO 1) while
disassembling it. The write site is the credit function.

## Phase 2.x implementation sketch (after RE TODOs done)

```
// Host side
struct CoopAssemblerState {
    std::array<uint32_t, kResourceTypeCount> pool;
    std::bitset<kBlueprintCount> unlocked;
};

// Wire: CraftBlueprintRpc { uint8_t blueprint_id; }
// Wire: AssemblerStateBroadcast { pool[]; unlocked_bits[]; }

// Host RPC handler:
void on_craft_rpc(uint8_t blueprint_id, PeerId from) {
    auto& bp = g_blueprints[blueprint_id];
    if (g_assembler_state.unlocked.test(blueprint_id)) return;
    if (!can_afford(bp.cost, g_assembler_state.pool)) return;
    deduct(bp.cost, g_assembler_state.pool);
    g_assembler_state.unlocked.set(blueprint_id);
    broadcast_assembler_state();
    // engine-side: call engine's "unlock blueprint X" function so
    // the player entity can equip it
    engine_unlock_blueprint(blueprint_id);
}

// Client side: assembler UI hook
void on_craft_confirmed_locally(uint8_t blueprint_id) {
    if (is_coop_client()) {
        // Don't touch local engine state — send RPC and wait for
        // broadcast.
        send_craft_rpc(blueprint_id);
        show_pending_indicator();
    } else {
        // Single player OR host — call engine's craft path directly.
        engine_craft(blueprint_id);
    }
}

// Disassembler shot credit:
// Hook the credit function. On client, redirect the credit to "send
// disassemble-shot RPC to host". On host, just let the engine code
// run normally — the credit lands in the host's pool which is the
// only pool.
```

## What is NOT in scope

- Per-player resource pools. Both players share host's pool.
- Per-player blueprint unlocks. Once unlocked, both have access.
- "Trading resources between players" UI. The pool is shared; no
  trading needed.
- Per-player crafting cooldowns / locks. The host serializes by
  arrival order; the loser sees their RPC come back with a "did
  not commit, blueprint already unlocked / not enough resources"
  result and the UI updates accordingly.

## Relation to other scope items

- **Phase 2.0 input replication** closes the disassembler-fire case
  for free (it's just `fire` button replication, like any other
  weapon).
- **Phase 2.x equipment-state replication** (O3 fix) closes the
  "newly unlocked weapon equipped" case — once the host has the
  unlock state replicated, the per-frame equipped-weapon-id slot
  carries the equipped item.
- **Phase 3.x unified entity replication** closes the dismantle-target
  state replication — the host's dismantle-target entities animate
  their "destroyed" state once, the broadcast covers it. Without
  Phase 3.x, the targets would visibly desync between players.

So the Assembler/Disassembler depends on Phase 2.0 + Phase 2.x +
(partially) Phase 3.x. Pure assembler work is ~1 week on top of those
prerequisites.

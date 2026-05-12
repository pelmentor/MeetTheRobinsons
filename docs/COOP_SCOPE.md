# Coop scope (Principle 5: Minimum viable subset)

This document defines the SCOPE of 2-player LAN coop for *Meet the
Robinsons*. **Anything not listed here is NOT in scope** and should not
drive code or replication decisions.

> Architectural principle 5: *"Anything not in scope is NOT replicated.
> Test every 'should we replicate X?' against scope."* — see
> [CLAUDE.md](../CLAUDE.md) and [ARCHITECTURE.md](ARCHITECTURE.md).

---

## In scope

### Players

- **Two players, host-authoritative, LAN UDP, same room.** Local
  same-machine dual-launch is NOT in scope (Disney mutex blocks it; see
  memory notes).
- Each player has their own Wilbur entity (P1 = engine wilbur, P2 = orphan
  spawned via `entity_factory_construct`).
- Per-player state: position, velocity, animation state, health, current
  weapon, current ability mode (e.g. T-beam, scanner), interaction
  target.

### Movement

- Walking, running, jumping, crouching, swimming if present.
- Wilbur's full movement subsystem ticks on both wilburs: ground
  follower, ledge controller, ladder controller, slide controller,
  jump scanner, climb controller, etc.
- Per-player movement input — P1 reads local input; P2 reads network
  input (replicated from peer).
- Per-player physics state — ground contact, collision capsule, etc.

### Interaction

- Picking up items (pickups, props, weapons).
- Using interactables (T-beam targets, lever pulls, scripted triggers).
- Receiving damage / dying / respawning.
- Talking to NPCs (scripted dialogue — if P2 talks, P1 sees it).
- Combat: aim, fire, throw, melee.

### Assembler / Disassembler (resources + blueprint crafting)

**Added 2026-05-12** after user clarification on MTR's actual progression
loop.

MTR's progression revolves around two devices:
- **Disassembler gun** — any player shoots dismantlable objects to break
  them into resources.
- **Assembler device** — level-placed kiosk; player walks up, opens UI,
  spends resources from the pool to unlock new weapons / cheats /
  gadgets that become permanently usable for the rest of the run.

Coop semantics:
- **Resources are a single shared pool**, stored host-side. Either
  player's disassembler shots add to the same pool.
- **Unlocked-blueprint set is shared, host-side**. Once anyone crafts
  blueprint X at any assembler kiosk, both players can equip and use
  the unlocked item.
- **Assembler UI is per-player on the activating player's screen.**
  When a client interacts with a kiosk, the craft menu opens on the
  client's screen only. The host does NOT see a menu — the host just
  sees the client's Wilbur standing next to the kiosk. When the client
  confirms a craft, an RPC is sent to the host; the host validates
  resources, deducts the pool, sets the unlocked flag, and broadcasts
  the new state. Concurrent kiosk usage by both players is allowed
  (each has their own menu instance on their own screen) — the host
  serializes craft commits in arrival order.
- **Disassembler shot crediting**: client's disassembler shot is an
  ordinary `fire` input replicated via Phase 2.0 input replication. The
  host's engine simulates the shot on the orphan, the engine's own
  disassemble-object code runs once on the host, and the resource
  credit happens once in the host's pool.

Why this is in scope: the assembler/disassembler IS the core progression
loop of MTR. A coop session that doesn't replicate the resource pool +
blueprint unlocks would mean P2 can never get new weapons and is
permanently underpowered. The mechanism reuses the same building blocks
as the rest of coop (host-authoritative state + per-player UI overlay +
RPC commits) — no new architecture needed.

Design phase: Phase 2.x (alongside equipment-state replication). RE
TODOs:
- Locate the resource-pool global (probably in save struct or
  `dword_*` near other progression state).
- Locate the unlocked-blueprint bitmask global.
- Locate the assembler-UI screen entry + craft-confirm callback.
- Locate the disassembler-shot resource-credit function (the engine's
  "object X was dismantled, +N resources of type T" path).

See [research/findings/coop-assembler-disassembler-scope-2026-05-12.md](../research/findings/coop-assembler-disassembler-scope-2026-05-12.md)
for the detailed design + RE TODO breakdown.

### Camera

- **Each player has their own camera**, but ONLY on their own machine.
  P1's screen follows P1's wilbur; P2's screen follows P2's wilbur. No
  split-screen on one machine.
- Per-player FreeCam (if both players want it).
- Per-player UI overlays (HUD, mod menu).

### Scripted content

- Level scripts (`.sx`) keep running normally. Triggers fire when EITHER
  player enters them (or with per-script semantics if the script
  distinguishes — TBD per-script).
- Cutscenes: when the engine triggers a cutscene, BOTH players see it on
  their screens. P2's avatar may be visible in the cutscene area
  depending on staging. **Skip semantics: either player pressing skip
  ends the cutscene for BOTH** (added 2026-05-12). Triggering side is
  the host's `.sx` VM regardless of which player crossed the trigger
  zone. See [research/findings/coop-cutscene-sync-2026-05-12.md](../research/findings/coop-cutscene-sync-2026-05-12.md).

### Entities (NPCs + enemies + bosses + interactables)

**Added 2026-05-12** after live LAN test feedback.

- **All scripted entities are replicated** — combat enemies, bosses,
  dialogue NPCs (Laszlo, Mildred, Lewis as NPC), props that script
  manipulates, T-beam targets, lever-pull-results.
- **Host-authoritative AI sim**: the host runs the `.sx` VM and AI
  state machines for ALL replicated entities. The client does NOT
  run AI for them — it receives per-tick state updates and renders.
- **Wire format**: per-entity (entity_id, position, orientation,
  anim_node, small bag of state — health/target/action). Delta-
  compressed. Per-entity send-rate gating based on visibility +
  activity.
- **Level-load manifest**: host enumerates spawned entities on
  level-load completion and broadcasts the list (entity_id → class
  → initial state). Client materializes orphans for each. After
  that, only state deltas flow.

Why this is in scope: MTR's coop story requires boss fights and
combat encounters where BOTH players see the same enemy in the same
state. Once that replication exists, dialogue NPCs use the same
mechanism with fewer state vars — they fall out for free. The
"per-client AI sim with drift" approach (initial 2026-05-12 design,
revised same day) doesn't work for boss fights.

Design phase: Phase 3.x, after Phase 2.0 (input replication) ships.

### Save/load

- Save game is the *host's* — P2's transient state is reconstructed from
  the host's save on session start. No per-player save slots.
- Loading a saved game starts a new coop session at that save point.

---

## Out of scope

### Players

- More than 2 players.
- Wide-area / internet multiplayer. LAN UDP only.
- Same-machine dual-launch (Disney mutex blocks it).
- AI-controlled P2 (no bot mode).

### Movement

- Server-side rewind for movement reconciliation (we trust the
  authoritative host).
- Lag compensation beyond simple position interp + small input delay.
- Predictive client-side movement with reconciliation. Plain interp is
  acceptable for the LAN latency target.

### Interaction

- Per-player inventory split. Both players share the host's inventory.
  (Until in-scope reason to split is identified.)
- Networked mini-games (DigDug, ChargeBall, etc.) — these stay
  single-player on the host's screen. P2 watches or is locked out.
- Networked drivable vehicles beyond what the player wilbur uses
  (T-beam etc. are in scope; if there are dedicated vehicle entities in
  certain levels, treat them per-level).

### Camera

- Split-screen single-machine.
- Per-player ortho / scripted camera divergence beyond what's natural
  (each follows their own wilbur).

### Scripted content

- Mission-script per-player branching (no per-player mission state).
- Cutscene per-player editing (cutscenes play as-shipped, possibly with
  P2's wilbur visible in-frame).

### Save/load

- Per-player save slots.
- Save-game-import-from-SP-to-coop deeper than "load this save file at
  session start".
- P2 joining a session NOT at save-load (mid-level join). Out of scope
  for v1; future consideration.

### General

- Custom Lua / scripting layer (MTA-style). We have one game and one
  path; no extensibility layer needed.
- Networked mod menu / shared debug overlays. Each player's mod menu is
  local.
- Anti-cheat. LAN coop with trusted friends; not a security concern.

---

## Application to current work

### Phase 2 (current — orphan keep-alive)

In scope: making the orphan a structurally complete second wilbur that
ticks safely in-process on the host. This is the foundation for Phase 1
networking — the host needs a working second wilbur before P2's net
state can be applied to it.

The 21-key +0xCCC registry mirror (b7.13) IS in scope — these keys hold
per-wilbur state that the movement / collision / health subsystems
consult. Without the mirror, those subsystems crash on the orphan.

The `coop_orphan_filter` was RETIRED 2026-05-12 (after sustained-soak
validation confirmed `coop_registry_mirror` + per-site
`coop_vibrate_route` are sufficient). Future per-site routes (C2, C3,
...) follow the same template as `coop_vibrate_route`. See
[research/findings/coop-filter-retirement-plan-2026-05-12.md](../research/findings/coop-filter-retirement-plan-2026-05-12.md).

### Phase 1 (designed — networking layer)

In scope per the design doc: authoritative-host UDP, per-player input
routing via b2-rem-2 component thunks (revised per MTA precedent), state
sync at ~13 Hz with interp.

NOT in scope: any of the "out of scope" items above.

### Future scope-creep checks

When tempted to add a feature, check this doc. If the feature isn't
listed in "in scope", it's NOT in scope — either skip it or argue
explicitly for adding it to scope (and update this doc).

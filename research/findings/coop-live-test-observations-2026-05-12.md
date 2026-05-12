# Coop live-test observations — 2026-05-12 evening

Captured during first user-driven manual LAN coop test, after Phase 1.6
gap-close + Step 7 LAN harness + windowed-mode infrastructure shipped.
Both Wilburs running locally via `coop-host.bat` + `coop-client.bat`,
windowed 1280×720, host had focus during interaction.

Each observation below is a known limitation of the current wire format
+ replication scope. None are bugs in the shipped code; they are
expected effects of "we only replicated position so far". Each maps to
a specific upcoming phase.

## What works (confirmed by this test)

- **Position replication**: each Wilbur sees the other Wilbur's
  position moving in real time. The `PoseSnapshotBody` packet (60 B at
  Phase 1.4a) carries x/y/z + yaw and the receive-side interp pulse
  applies it. Network throughput from the autonomous LAN soak test:
  ~2000–4500 packets sent + ~2000–2600 received per side over a
  1800-frame soak — consistent with the design.
- **Auto-spawn of the P2 orphan**: each side correctly spawns the
  other player's character (orphan entity) on first packet receipt
  (Phase 1.4d auto-orphan-spawn).
- **`fires_p2 = 0` under live peer**: the engine never polled `dev_p2`
  even with a real peer (Phase 1.6 gap-close invariant holds
  end-to-end).

## What does NOT work yet (gameplay observations)

### O1 — Remote Wilbur slides without animation

**Observed**: on the host's screen, the client's Wilbur is sliding
through space without playing run/walk/idle animations. Same in
reverse on the client's screen.

**Root cause**: the wire format carries position only. The orphan
entity's animation graph (engine class `Wilbur` with its Gamebryo-style
anim controller per
[research/findings/animation-system.md](animation-system.md)) is
driven by the local animation tick, which sees a sequence of position
updates but no anim state. The orphan never receives "go to RUN" or
"play JUMP" inputs.

**Fix path (Phase 2.0)**: either
- (a) **Replicate input** — send remote player's button/analog state
  (the 18 button-pair + 4 analog floats from the Phase 1.6 RE) via
  `write_p2_state` (the API is already stubbed in
  `controlmapper_dev`). The remote Wilbur's ControlMapper then drives
  its own animation graph from "P2's input". MTA precedent: per-frame
  pure-sync packet at `multiplayer_keysync.cpp` (handles all
  movement/anim via raw controller state).
- (b) **Replicate animation state** — send the current anim graph
  node ID + blend weights. Smaller wire, but requires anim-graph
  state serialization (more RE).

Approach (a) is the audit-recommended path. Wire format spec already
matches (`P2InputState` struct at
`controlmapper_dev.h` lines 105–117: 18 button "curr" bytes + 4
analog floats at `dev_p2+0x34..+0x43`).

### O2 — NPCs in different positions on each Wilbur

**Observed**: each Wilbur sees NPCs (Lewis, Mildred, Laszlo, robots,
etc.) at different positions. Each client is running its own NPC sim
independently.

**Root cause**: NPCs are not replicated. Each Wilbur's engine runs the
.sx AI VM independently (per
[research/findings/ai-script-vm.md](ai-script-vm.md)), so identical
initial state diverges within seconds due to floating-point drift +
local input handling differences.

**Fix path (Phase 3.x — unified entity replication, REVISED 2026-05-12
per user feedback)**: this IS in scope. Originally framed as "out of
MVP per MTA precedent" — but MTR's situation differs from MTA in a
decisive way: **MTR has combat encounters with bosses and enemies that
the coop story REQUIRES both players to see in the same state.** Those
combat enemies HAVE to replicate or the coop is unplayable for the
core missions (boss fights, scripted enemy encounters).

Once host-authoritative enemy replication is built — which it must be
— simple dialogue NPCs (Laszlo, Mildred, Lewis when not gameplay
character) are the same problem with fewer state variables. They fall
out for free from the unified entity-replication mechanism.

Design (Phase 3.x):
- **Host-authoritative AI sim.** The HOST runs the .sx VM + all AI
  state machines. The CLIENT does NOT run AI scripts for the
  replicated entities — they are pure render-receivers.
- **Per-entity state replication**: position + orientation + anim
  node ID + a small bag of script-state variables (health, target,
  current-action). Wire-format: ~32–64 B per entity per tick, with
  delta-compression and per-entity send-rate gating
  (visible-to-other-player entities update more often than
  distant/scripted-idle ones).
- **Per-level "entity manifest"**: at level-load, the host enumerates
  spawned entities and broadcasts the list (entity_id → class →
  initial state). The client materializes orphans for each. After
  that, only state updates flow.
- **Cutscene / scripted-only NPCs**: same path, just lower update
  rate when not actively animating.

Per MTA's precedent the closer pattern is **CClientPed** + the
`peds_pure_sync` packets for in-game NPCs — MTA DOES replicate ped
state for the subset that are "active" in the level. The "NPCs not
replicated" line from earlier was overstated; the precedent is
"replicate active entities, don't replicate ambient world peds". MTR's
NPCs are all scripted/named — they all qualify as "active".

This is Phase 3.x work — substantial but unavoidable for boss fights
and combat. Phase 2.0 (input replication) ships first, Phase 3.x is
the next big design block after.

### O3 — Wrong weapon / gauntlet on remote Wilbur

**Observed**: on the host's screen, the client Wilbur shows a
different weapon/gauntlet equipment than the client actually has
equipped. Same in reverse.

**Root cause**: equipment state is per-process. Each engine maintains
its own "currently equipped weapon" state for the player entity. The
auto-spawned orphan on the host has whatever default equipment the
engine assigns at spawn-time, not what the remote player is actually
holding.

**Fix path (Phase 2.x)**: add `equipped_weapon_id` +
`equipped_gauntlet_id` (or whatever the engine's slot system is) to
the pose packet. Each side reads the remote side's equipment and
calls the engine's "set equipped" API on the orphan. Requires RE'ing:
- The engine's equipment-set function (probably called when the
  player presses weapon-switch buttons)
- The equipment ID enum (which weapon = which int)

Wire cost: 2 bytes per packet (1 byte per slot). Cheap.

**Per MTA precedent**: yes, MTA replicates equipped weapon ID — see
`CWeapon` sync in MTA's `multiplayer_keysync.cpp`. Same pattern fits
here.

### O4 — Level transitions break the coop link permanently

**Observed**: if one Wilbur uses an in-game tube to transition to a
different level, that Wilbur is "lost forever" from the other player's
view. Returning to the original level, the other Wilbur is no longer
there.

**Root cause**: each engine instance independently loads/unloads
levels. When player A transitions, A's engine destroys all entities
of the current level + loads the new level. A's orphan-of-B is
destroyed. A's MtrPlayerManager loses the orphan wrapper. B keeps
sending position updates but A's recv path can't find a Remote wrapper
to push them into.

When A returns to the original level, A's engine spawns a fresh
"empty" world. The orphan-of-B is not re-created because the
auto-spawn-probe latch `m_peer_seen` is one-way (set true once, never
reset).

**Fix path (Phase 1.7 or 2.0)**:

1. **Short-term**: reset `m_peer_seen` on level-load completion so
   auto-spawn fires again. Cost: a hook on the engine's level-load
   completion callback. Effect: when A returns to the original level,
   `auto_spawn_p2` runs again and the orphan is recreated. Position
   updates resume.

2. **Longer-term**: replicate the "currently loaded level ID" in the
   pose packet. If A and B are in different levels, both sides know it
   and don't try to interpolate the other's position (which is in a
   coordinate space A's engine has no entity for). When they re-enter
   the same level, the orphan respawns and replication resumes.

3. **Best-case**: synchronize level transitions — when A enters a
   tube, both A and B transition simultaneously. Requires either
   (i) the tube interaction propagates to B's engine ("you're going
   to level X now") which is invasive, or (ii) the user agrees to
   "wait at tube until both players are there". MTA's session lobby
   model is similar.

Item (1) is the minimum-viable patch. Item (2) is the proper "show
in UI which level the other player is in". Item (3) is the
coop-design decision (per the Phase 2.0 design session).

### O5 — Both Wilburs slide in each other's windows (general)

Already covered by O1. The slide is "position updates without animation
state" — pose interp ships the position fine but the Wilbur's anim
controller has no input to drive a walk/run cycle, so the model T-pose
slides along the path.

## Roadmap mapping

| Observation | Phase | Effort | In MVP scope? |
|---|---|---|---|
| O1 — anim slide  | Phase 2.0 (input replication) | weeks | yes |
| O2 — NPC desync  | Phase 3.x (unified entity replication) | weeks–month | **yes** (REVISED 2026-05-12 — boss fights require it) |
| O3 — wrong weapon | Phase 2.x (state replication) | days  | yes (extends Phase 2.0) |
| O4 — level break  | Phase 1.7 (level handling)  | days for short-term, weeks for proper | yes |

## Next session priority

Per the user redirect 2026-05-12 EOD (in-engine autoload via cmdline)
the autonomous-test path is being rebuilt without the DI-injection
crutch. AFTER autoload ships, the priority order shifts:

1. **O4 short-term fix** (reset `m_peer_seen` on level-load completion)
   — small, unblocks any test that crosses a tube.
2. **O1 + O3 — Phase 2.0 input replication design**. Implement
   `write_p2_state` caller path: read local Wilbur's button/analog
   state from `dev_p1`, send via the existing UDP transport, peer
   side calls `write_p2_state` on `dev_p2`. Both anim AND weapon-state
   fall out naturally because the engine's own ControlMapper drives
   them from `dev_p2` once it sees real input bytes.
3. **O4 long-term** — replicate currently-loaded-level-id and gate
   render of remote orphan on level match.
4. **Phase 3.x — unified entity replication (O2 + boss fights + all
   scripted NPCs)**. Host runs the .sx AI VM authoritatively; client
   receives per-entity state updates. Same mechanism covers combat
   enemies AND dialogue NPCs (the latter come free once the former
   exists, since both are .sx-script-driven entities). Per-entity
   manifest at level-load + delta-compressed state updates. Big
   design block — second-biggest after Phase 2.0 input replication.
   Detailed design needed: wire format, manifest sync, latency model.

This is the recommended Phase 2.0 + Phase 1.7 split. None of the
observations are bugs in the Phase 1.6 ship; they are the consequence
of "MVP shipped position-only, the rest is planned work".

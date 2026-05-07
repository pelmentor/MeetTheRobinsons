# M4 — Wilbur transform interp shipped

Per-render-frame interpolation of the player entity's pos+rot, paired with
the M2.3 save-write-restore fence (since this is the first interp client
that mutates state sim reads back).

Stacks with M3.1 view interp: M3 makes the camera fluid, M4 makes Wilbur's
body fluid. With both on at 240/60, the avatar moves smoothly at render
rate, while physics + animation + AI tick at 60 Hz.

## Player entity layout (verified)

Found via static RE of `ai_resolve_nav_vector` (0x5B9050) reading
`*((float*)result + 22)` = entity+0x58 = pos.x, and
`entity_transform_tick` (0x4B9F60) writing 9 floats at entity+0x70:

```
entity + 0x58 (12 bytes): pos.x, pos.y, pos.z       (3 floats)
entity + 0x70 (36 bytes): rotation matrix 3x3       (9 floats, row-major)
```

The entity returned by `entity_lookup_by_name_retry("player", 1)`
(0x5AC8F0) is the canonical player handle for actor mode. Mini-game
classes (`wilburDigDug`, `miniHamsterPlayer`, `WilburDriver`) may have
different layouts — but M4 is auto-vetoed in mini-game mode by the
existing `effective_is_throttling()` gate (M1.5).

## Architecture

### Snapshots

```
PlayerSnap { pos[3], rot[9], qpc, valid }
prev_player <- curr_player   (shifted at each capture)
curr_player <- entity        (captured POST sim_aggregator orig)
```

Player snapshots are gated by `g_curr_player.valid` and the dirty-bit
flow is implicit: only POST orig sim sets curr_player. When sim is
throttle-skipped, neither snapshot moves; alpha keeps growing toward 1.

### Save-write-restore fence (M2.3)

The view + world matrix globals (M3.1) don't need a fence — sim doesn't
read them. The player entity DOES — `entity_transform_tick` reads its own
state to compute next-frame derivatives.

```
hk_sim_aggregator (PRE orig):
    pre_sim_restore_player()       // copy saved_player -> entity
    if (skip): return 0             // (saved was already restored)
    rc = orig
    on_sim_tick_post()              // M3 dirty
    post_sim_capture_player()       // shift prev<-curr; capture curr from entity

hk_camera_apply_all_active (POST orig):
    [M2 view snapshot]
    [M3.1 view interp write]
    if (player_interp + has 2 + !teleport + throttling):
        capture entity -> saved_player
        save_valid = true
        write lerp+slerp(prev, curr, alpha) -> entity
```

Save happens INSIDE the camera_apply hook (the same place we write the
interp). saved_player holds the "real sim state" we'll restore PRE-next-
sim. The write happens after save, so the entity ends the render frame
in interp state — render-thread consumers (the visible Wilbur body)
see the smooth transition.

### Teleport detection

Per-sim-tick translation delta > threshold (default 10.0 units) → mark
this sim window as a teleport, snap (no interp this window). Catches
respawns, scripted jump-cuts, level-load placements. Tunable via slider.

## Interp math

Pos: linear lerp.
Rot: 3x3 → quaternion (Shepperd's method, reused from M3) → slerp →
quaternion → 3x3. Values written back in the engine's row-major float[9]
layout.

## Cached player handle

`entity_lookup_by_name_retry("player", 1)` is called on first need, on
null cache, and every 60 render frames. The 60-frame refresh covers
level transitions where the entity pointer can change without an
intervening null.

## Toggles

- **Tools → Decouple → "Enable player transform interp (M4)"** — default OFF
- **Tools → Decouple → "Teleport threshold"** slider (1..100 units)
- FPS overlay status flag: `PLR:ON`/`PLR:off`

## Validation

To confirm M4 is doing its job:

1. Throttle ON, target=60, FPS limit 240
2. M3 view interp ON
3. M4 player interp **OFF**: walk forward; avatar steps at 60 Hz (visible
   stutter against fluid camera).
4. M4 player interp **ON**: avatar moves at 240 Hz (smooth).
5. Die / respawn: should snap (no glide). `Teleports` counter increments.

## Inherent latency

Same ~1 sim-window latency as M3 — render shows lerped state behind the
freshest sim. For Wilbur this is rarely perceptible because the player
controls input through the sim path; visual feedback is consistent with
input. For gunplay against fast-moving targets, ~16 ms latency may feel
slightly heavy; M3.2 aim-mode snap (deferred) addresses this when in aim
mode.

## Performance

- Pos lerp: 3 lerps = ~10 fp ops
- Rot slerp: same as M3 (~50 fp ops + 1 acos + 2 sin)
- Save (memcpy): 48 bytes
- Restore (memcpy): 48 bytes
- Per render frame: ~80 fp ops + 1 acos + 2 sin + 96 bytes memcpy = ~3 µs

Combined with M3 view interp: ~8 µs per render frame, still 0.2% of a
4 ms 240-Hz frame budget.

## Files touched

| File | Change |
|------|--------|
| `src/mtr-asi/include/mtr/interp.h` | M4 API + teleport threshold + writes counter |
| `src/mtr-asi/src/interp.cpp` | PlayerSnap, lookup cache, snapshot/save/restore/write, teleport detect, slerp reuse |
| `src/mtr-asi/src/sim_decouple.cpp` | hk_sim_aggregator: PRE pre_sim_restore_player + POST post_sim_capture_player |
| `src/mtr-asi/src/menu.cpp` | new toggle + counters + threshold slider; PLR flag in overlay |

## See also

- [`decouple-m3-view-interp-2026-05-07.md`](decouple-m3-view-interp-2026-05-07.md) — M3 sister
- [`decouple-m2-snapshot-infra-2026-05-07.md`](decouple-m2-snapshot-infra-2026-05-07.md) — snapshot framework
- `memory/project_player_system.md` — entity offsets reference
- [`high-fps-decoupling-plan.md`](high-fps-decoupling-plan.md) §M4 — original plan

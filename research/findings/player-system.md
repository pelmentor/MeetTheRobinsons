# Player (Wilbur) system

Architecture of the player avatar — class hierarchy, lookup, camera,
input bindings, weapons, health/damage, and movement states.

## TL;DR

1. **Wilbur is the only player avatar**, with **4+ game-mode subclasses**
   that swap C++ class + AI script while keeping the same model
   (`avatars/wilbur.mdb`).
2. **Player lookup is by name `"player"`** via
   `entity_lookup_by_name_retry("player")` (`0x5AC8F0`). Every subsystem
   that needs the player position calls this — AI scripts via
   `ai/nPlayerLoc`, cameras via PathCam target queries, mini-game UI,
   etc.
3. **Player camera = PathCam** (`0x58E6C0` ctor, `0x58C910` tick).
   Path-following with TWO path slots (primary + secondary) plus a
   smoothing pre-tick at `pathcam_smooth_pretick` (`0x58C000`) that
   uses the **same hardcoded 0.003 sec step as physics**.
4. **Input is via `ControlMapper`** with mode-specific schemes
   (default / DigDug / MiniHamster). Action names found:
   `MoveForward(Normalized/Allowed)`, `moveup`, `Jump*`, etc.
5. **Wilbur exposes ~10 script commands** for runtime control:
   `WilburAddWeapon`, `WilburRemoveWeapon`, `WilburAbortTargeting`,
   `PlayWilburFaceAnim`, `StopWilburSecondaryAnims`,
   `ResetActionIFAnim1AndHandlers`, etc.
6. **6 weapons** ship as `.dbl` files: `disassIRR` (Disassembler),
   `levIRR` (Levitator), `tracer`, `bhole` (Black Hole), `lobbed`,
   `laser`.

## Game modes (same Wilbur, different class+script)

Each mode is a `class=...; model_name=avatars/wilbur*; ai=avatars/...sx`
spawn template. Mode-specific scripts let one C++ avatar carry
mode-specific behavior.

| Mode | C++ class | AI script | Description |
|---|---|---|---|
| Main adventure | `actor` (or unnamed default) | `avatars/wilbur.sx` | Standard third-person platformer |
| Variant 2 | (same?) | `avatars/wilbur2.sx` | Possibly a level/chapter variation |
| Variant 3 | (same?) | `avatars/wilbur3.sx` | Possibly a level/chapter variation |
| Variant 4 | (same?) | `avatars/wilbur4.sx` | Possibly a level/chapter variation |
| Hamster mini-game | `miniHamsterPlayer` | `avatars/wilburMiniHamster.sx` | Hamster-ball mode (also used for family members: UncleGaston / franny / GrandpaBud) |
| DigDug mini-game | `wilburDigDug` | (mode-specific) | Top-down DigDug clone |
| Driver mini-game | `WilburDriver` | (mode-specific) | Vehicle-driving segment |

So the same `wilbur.mdb` mesh is reused across modes; mode-specific
classes + scripts give different controls + behavior. Family-album
characters (`UncleGaston_Hi`, `franny_Hi`, `GrandpaBud_Hi`) reuse the
`miniHamsterPlayer` class with their own meshes.

Wilbur-specific config files:
- `Wilbur.gfg` — game-feature graph?
- `Wilbur.bld` — build?
- `Wilbur.hdr` — header?

## Player lookup — `entity_lookup_by_name_retry` (0x5AC8F0)

Every subsystem that needs Wilbur's position uses this lookup pattern:

```c
int entity_lookup_by_name_retry(const char* name, int flags) {
    result = entity_lookup_by_name(name);    // sub_5C87A0 (SecuROM-style)
    if (!result) {
        result = entity_lookup_by_name(name);
        if (!result) result = entity_lookup_by_name(name);  // 3rd try
    }
    return result;
}
```

Calls the actual lookup up to 3 times — likely because the entity
registry is multi-level (e.g., level-bound + global-bound buckets) and
each retry searches a different bucket.

The lookup key is the literal string `"player"`. So Wilbur's spawn
template includes `name=player;...`. From the spawn-template strings
we found: not-yet-located on disk; the engine emits this name during
ctor.

Returned struct layout (offsets observed in `ai_resolve_nav_vector`):
- `+0x58` (= `*(p + 22 floats)`) → **pos.x**
- `+0x5C` → pos.y
- `+0x60` → pos.z

So the player entity uses the same `+0x58..+0x60` position layout as
other actor entities (already documented in entity-system.md).

## Camera — PathCam

`PathCam_ctor` (`0x58E6C0`) builds a **760-byte** structure (offsets
into `this` go up to +759). Key fields:

| Offset | Field |
|---|---|
| `+0x00` | vtable (= 0x6CA420) |
| `+0x10` | outer camera object ptr |
| `+0x38` | "frame begin" callback vtable |
| `+0x3C` | primary-path vtable (fallback `*(this+16)+116`) |
| `+0x40` | secondary-path vtable (fallback `*(this+16)+120`) |
| `+0x50` | view matrix (16 floats) |
| `+0x150` (= 336) | smoothed cam pos.x |
| `+0x15C` (= 348) | smoothed look-at pos.x |
| `+0xD0` (= 208) | secondary matrix combined via `sub_41A380` |
| `+0x110` (= 272) | output matrix copied via `matrix4_copy` |
| `+0x190` (= 408) | action-handler #1 (input target) |
| `+0x1C4` (= 452) | action-handler #2 (input target) |
| `+0x16C..+0x1A4` | spring-damper params (max distance², reaction rates) |
| `+0x28C` (= 651) | nested object vtable (= 0x6CA3F4, secondary state) |
| `+0x2F4..+0x2F7` | state byte cluster (enabled flags, mode toggles) |

### Pre-tick smoothing — `pathcam_smooth_pretick` (0x58C000)

```c
// Read target position + look-at + up from active path
path->vtable[2](&target_pos);
path->vtable[3](&look_at);
path->vtable[4](&up_vec);

// Smooth current cam pos toward target (critically-damped follow)
// HARDCODED 0.003 sec step -- same fixed-step issue as physics
//   threshold check: jump-to-target if distance² > max² (cam+101)
this->cam_pos = sub_405630(target_pos, this->cam_pos, 0.003, this->react_rate);
this->look_at = sub_405630(look_at, this->look_at, 0.003, this->react_rate2);

// Compute basis: forward = look_at - cam_pos, up = up_vec, right = forward × up
this->right   = ...;
this->up      = ...;
this->forward = ...;

// Action-handler timers (3.384sec windows)
if (action1_active && (timer1 < 0 || timer1 >= 3.384)) {
    action1_handler->vtable[3](this, &out, &action1);
}
if (action2_active && (timer2 < 0 || timer2 >= 3.384)) {
    action2_handler->vtable[3](this, &out, &action2);
}

// Build final look matrix at this+72..+78
```

This is a standard **camera-follow with critically-damped springs**.
The 3.384-sec constant matches level fade times (also used in
`level_manager_init_load`).

### Per-frame tick — `PathCam_tick` (0x58C910)

```c
this->frame_begin->vtable[1]();          // pre-tick callback
pathcam_smooth_pretick(this);

primary_path = this->primary_path ?: outer_cam->path1;
primary_path->vtable[1](this);            // primary path computes view

sub_58BA30(this);                         // mid-tick (?)

// Combine local matrix with secondary matrix
sub_41A380(this+80, this+208);
matrix4_copy(this+272, &combined);

secondary_path = this->secondary_path ?: outer_cam->path2;
secondary_path->vtable[1](this);

// Final: write view matrix to outer cam at +0x10
matrix4_copy(outer_cam+0x10, this+80);

sub_58BC20(this);                         // late-tick
return sub_58BB50(this);                  // post-tick
```

The `outer_cam+0x10` is then read downstream by
`camera_apply_all_active` and propagated to `g_d3d_view_matrix_global`
(0x724C10). This is the path the **FreeCam** mod overrides (we hook
*after* `camera_apply_all_active` rather than mid-PathCam, since
script-cams and death-cams take over at runtime).

## Camera modes

Strings reveal the camera-state machine:

| String | Likely state |
|---|---|
| `CameraJumpModeOnFrame` / `CameraJumpModeOffFrame` | Jump-mode toggle (camera lifts when Wilbur jumps) |
| `CameraPelvisModeOnFrame` / `CameraPelvisModeOffFrame` | Pelvis-attached vs default attachment |
| `CameraAutoCenterFrame` / `CameraAutoCenter180` / `CameraAutoCenterTime` | Auto-center behind Wilbur after N frames or after 180° turn |
| `CameraBot_*` | Helper bot for safe-spawn / look-at / spotlight (level-design-time tooling) |
| `SetPathCamTargetingBehavior` | Switch into aim/lock-on mode |
| `wilburPitchMultiplier` | Mouse-look pitch sensitivity |

So the camera has multiple input-driven modes (jump, pelvis, auto-center,
targeting) plus a per-level "CameraBot" helper that may be a debug/dev
tool.

## Input — `ControlMapper`

The engine uses a `ControlMapper` to bind inputs to actions. Found
script bindings:
- `DigDugSetControlMapperScheme` — switch to DigDug control scheme
- `MiniHamsterCreateControlMapper` — initialize hamster-mode mapper
- `FreeDefaultControlMapper` — release the default mapper
- `useTestPlayerControlMapper` — debug toggle

Action names found:
- `MoveForward`, `MoveForwardNormalized`, `MoveForwardAllowed`
- `moveup`
- `Action`, `ActionHandler`
- (Jump bindings inferred from `Jump*` anim states, exact action names not yet located)

`input_get_active_source_for_player` (`0x56F290`) is an **orphan** —
only one xref. So the player-input dispatcher pattern is unwired in
retail Wilbur; the actual input arrives via a different path
(`ControlMapper` directly polls or via dispatch from a screen/scene
state we haven't fully traced).

## Movement states

From animation-state strings, Wilbur has the following ground states:

| Anim state | Description |
|---|---|
| `Idle2`, `Idle3`, `Idle4` | Multiple idle anim variants |
| `WalkGrabLeft/Right` | Wall-grab walking (ledge climbing?) |
| `Slide`, `slidestart`, `slidestop` | Slide-mode (`SlideController` exists) |
| `WalkGrabLeftStance/RightStance` | Wall-grab stance |
| `*Jostle` variants | Stance-jostle (animation noise) |
| `JumpScanner` | Pre-jump collision scan |
| `JumpOn`, `JumpOff`, `JumpOver`, `JumpTo`, `JumpSmall` | Jump variants |
| `landedHit` | Landing reaction |

Speed-control script commands:
- `MoveGetMaxSpeed`
- `SetGroundMaxSpeed`, `SetGroundMaxSpeeds`

## Weapons

Six weapons identified (each is a separate `.dbl` asset):

| File | Name | Notes |
|---|---|---|
| `weapons/disassIRR.dbl` | Disassembler | Wilbur's primary tool ("IRR" = Inverse Reality Rifle?) |
| `weapons/levIRR.dbl` | Levitator | Move objects |
| `weapons\tracer.dbl` | Tracer | Bullet trail |
| `weapons\bhole.dbl` | Black Hole | Wide-area weapon |
| `weapons/lobbed.dbl` | Lobbed | Grenade-style projectile |
| `weapons\laser.dbl` | Laser | Beam weapon |

Script commands:
- `WilburAddWeapon(weapon)` / `WilburRemoveWeapon(weapon)` — inventory ops
- `WilburAbortTargeting` — cancel current target lock
- `SetPathCamTargetingBehavior` — switch camera to aim mode
- `targeting`, `Tutorial_targeting*` — tutorial copy

## Health / damage

| String | Purpose |
|---|---|
| `health`, `healthAmount`, `healthBar`, `HealthTracker` | Health state + UI |
| `damage1` / `damage2` / `damage3` | Damage amounts (3 tiers) |
| `damageType`, `damageAmount`, `damageRadius`, `damageTime` | Damage type config |
| `damageTypeCC`, `damageSpecial`, `damageExplosive` | Damage flavors |
| `Shield_SetInvulnerableToAll`, `Shield_AddInvulnerable` | Shield system |
| `CH_Invulnerable`, `Scn_Cheats_Invulnerable`, `SetInvulnerable`, `GetInvulnerable` | Invulnerability flag (cheat-controllable) |
| `dieSound`, `dieOnEnd`, `DamageEnabled`, `DamageEnabler`, `DamageStateTracker` | Death + state |
| `DamageNotification`, `DamageRadius`, `DamageRadiusExceptActor`, `DamageSurfaceTracker` | UI + spatial damage |
| `landedHit` | Fall damage |

The damage system is config-driven (3 tiers by index, with type +
amount + radius + interval) — typical for an action game. Cheats
(`Scn_Cheats_Invulnerable`, `WilburCheatsEnabled`) can turn it off.

## AI navigation queries about the player

`ai_resolve_nav_vector` (`0x5B9050`) — the AI vector-resolver — handles
five `ai/n*` keys, one of which is `ai/nPlayerLoc`:

```c
const char* ai_resolve_nav_vector(actor, key, ...) {
    if (key == "ai/nActorName")    { entity = actor_lookup(...);    relative_pos = entity[22] - actor[22]; }
    if (key == "ai/nInstanceName") { entity = instance_lookup(...); relative_pos = entity[14] - actor[24]; }
    if (key == "ai/nPlayerLoc")    { entity = entity_lookup_by_name_retry("player", 1); ... }
    if (key == "ai/nNamedPt")      { entity = named_point_lookup(...); ... }
    if (key == "ai/nWorldCoord")   { return fixed_world_coord; }
    return SECUROM_THUNK(+163190, ..., actor, key);
}
```

So **AI scripts can query the player's position via `ai/nPlayerLoc`**.
This is the canonical hook for "follow the player" / "flee from player"
behaviors. Combined with the bidirectional registry from the AI doc,
overriding the player position for AI is straightforward (replace the
result of `entity_lookup_by_name_retry` for the literal `"player"`).

## What's left to investigate

1. **Wilbur's class instance**: which spawn template creates him, who
   calls it, and where the resulting pointer is cached. (Probably set
   during level load and re-resolved each frame via `entity_lookup_by_name`.)
2. **Per-frame movement**: which C++ class method moves Wilbur each
   frame? Likely a `vtable[N]` on the actor, called from a screen /
   scene tick.
3. **Aim/targeting reticle**: the `WilburAbortTargeting` +
   `SetPathCamTargetingBehavior` chain implies a targeting state machine
   tied to weapon use.
4. **Family-album / mini-hamster mode**: `Player_Tallulah / Franny /
   GrandpaBud / AuntBillie / UncleArt / Trainer / ChargeBallChamp` —
   localization keys for player names. Confirms multiplayer/roster
   support; how does this map to gameplay?

## Anchors created this session

| VA | Symbol |
|---|---|
| `0x5AC8F0` | `entity_lookup_by_name_retry` (3-attempt wrapper) |
| `0x5C87A0` | `entity_lookup_by_name` (the actual lookup) |
| `0x58C000` | `pathcam_smooth_pretick` |
| `0x58C910` | `PathCam_tick` (already named) |
| `0x58E6C0` | `PathCam_ctor` (already named) |
| `0x5B9050` | `ai_resolve_nav_vector` |
| `0x56F290` | `input_get_active_source_for_player` (already named, orphan) |

## See also

- [`entity-system.md`](entity-system.md) — base entity layout (Wilbur is one of these)
- [`ai-script-vm.md`](ai-script-vm.md) — `.sx` scripts that drive Wilbur's behavior
- [`animation-system.md`](animation-system.md) — anim-state map (Idle/Walk/Run/Slide/Jump variants)
- [`systems-survey-2026-05-06.md`](systems-survey-2026-05-06.md) — top-level overview + 0.003 step caveat (also affects pathcam smoothing)
- [`frame-pacing.md`](frame-pacing.md) — fps caveat docs

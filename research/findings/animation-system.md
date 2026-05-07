# Animation system

How the engine's animation graph evaluates each frame: time controllers,
curve samplers, channel-state tracks, hide cascades, and matrix updates.
Gamebryo-derived runtime — track tree + driver/curve/instance pattern.

## TL;DR

- **Per-frame entry**: `anim_update_all_tracks` (`0x4E4B70`) called once
  from the simulation tick aggregator. It (1) advances every time
  controller, then (2) evaluates every active track in the global pool.
- **Per-track evaluator**: `anim_evaluate_track` (`0x4E4370`) is a
  **parent-first recursive** evaluator. Hide cascades down the parent
  tree; channel 0 of the track state is hard-wired to scene visibility
  (`(scene+104) bit 0` — same flag we ruled out for camera culling).
- **Curve format**: 28-byte segments, three interpolation types —
  LINEAR (`0x20`), CONSTANT/STEP (`0x50`), CUBIC BEZIER (default; uses
  Newton-Raphson to invert the time poly + Hermite eval for the value).
- **Time advance**: `anim_controller_advance` (`0x4E2B00`) uses the
  same hardcoded **0.003 sec step** as physics — 333 Hz fixed step,
  scaled by per-controller rate (`time += 0.003 * rate`). Same FPS
  caveat as physics.
- **Pool access via SecuROM**: tracks live in a SecuROM-managed pool;
  `anim_pool_get_at_thunk` (`0x4BD730`) gets entry by index. Pool head
  + count word at `unk_726D5C`.

## Per-frame call graph

From [`systems-survey-2026-05-06.md`](systems-survey-2026-05-06.md):
```
simulation_tick_aggregator (0x67F430)
└── anim_update_all_tracks (0x4E4B70)
    ├── anim_advance_time (0x4E2CA0)
    │   └── for i in 0..word_7264E8:
    │       └── anim_controller_advance (0x4E2B00)
    │           // time += 0.003 * rate; loop logic
    └── for i in 0..unk_726D5C[0]:
        └── instance = anim_pool_get_at_thunk(unk_726D5C, i)  // SecuROM thunk
            └── if !(instance->flags & 0x40000000):
                └── anim_evaluate_track(instance)
                    └── (recursive parent-first)
```

## Time controller — `anim_controller_advance` (0x4E2B00)

Runs once per frame for every active controller. Layout:

| Offset | Type | Purpose |
|---|---|---|
| `+0x00` | float | Current time |
| `+0x04` | float | End/clamp-high |
| `+0x08` | float | Start/clamp-low |
| `+0x0C` | float | **Rate** (can be negative for reverse) |
| `+0x10` | int | **Loop mode**: 1=clamp, 2=clamp+end-event, 3=ping-pong, 4=wrap |
| `+0x14` | byte | Flags (bit 0 = "stop on first stop event") |

Step:
```c
time += 0.003 * rate;          // hardcoded 0.003 sec, same as physics
switch (loop_mode):
  case 1: snap to start/end if past
  case 2: same + set bit 3 in flags (end-event raised)
  case 3: negate rate at boundary (ping-pong)
  case 4: wrap-around modulo
```

**Same hardcoded-0.003 caveat as physics.** Animations advance by 333 Hz
of "sim time" per render frame — at 60 fps real, anim plays at 5.5x
slow-mo; at 333 fps real, anim plays real-time. (Channel sample times
are interpolated from the controller's `time`, so this only matters
for the controller's own rate, not the curve interpolation accuracy.)

## Curve sampler — `anim_curve_sample` (0x4E2D80)

Resolves one curve at a given time. Curve segments are **28 bytes
each**, stored at `(this + 16 + 28 * idx)`:

| Offset | Field |
|---|---|
| `+0x00` | (segment hash / pad) |
| `+0x08` | segment count |
| `+0x0C` | **type flags** (interp via `flags & 0xF0`) |
| `+0x10` | time of segment start |
| `+0x14` | value of segment start |
| `+0x18` | (reserved) |
| `+0x1C..+0x24` | tangent control points (Bezier handle params) |
| `+0x2C` | time of NEXT segment (= this segment's end time) |
| `+0x30` | value of NEXT segment |
| `+0x34` | tangent-out for cubic |

The caller passes in/out the **current segment index** in `a3` —
sequential reads cache the index, so seeking is O(1) amortized.

Three interpolation types via `(seg.flags & 0xF0)`:
- **0x20 (LINEAR)**: `lerp(t, v_a, v_b)`
- **0x50 (CONSTANT/STEP)**: returns `v_a` (no interpolation)
- **default (CUBIC BEZIER)**:
  1. Compute Hermite/Bezier control coefficients from segment +
     `sub_4E2CE0`.
  2. **Newton-Raphson invert the time cubic** `((c3*t + c2)*t + c1)*t = a2`
     to find normalized parameter `t` at the requested time.
  3. Evaluate value cubic `((vc3*t + vc2)*t + vc1)*t + vc0`.
  4. Tolerance loop: 1e-5 with up to 3 stuck-iteration guard.

Same algorithm as Gamebryo's `NiAnimationKey::BezierKey` evaluation
and most authoring tools. Curves are exported pre-fitted from Maya/Max.

## Per-keyframe resolver — `anim_keyframe_resolve` (0x4E4220)

Called once per keyframe per track per frame. Caches: a keyframe stores
its last-sampled value and the last input time. If the controller's
current time hasn't changed (same frame as a previous read), return
the cached value. Otherwise, call `anim_curve_sample` and update cache.

State at `*this`:
- `+0x00` (lo word): channel index (0..31)
- `+0x04`: pointer to controller (= time source)
- `+0x0C`: cached value (float)
- `+0x10`: cached input time (float)

The "time changed" gate is what keeps the system O(active tracks) per
frame instead of O(active tracks × keyframes_per_track).

## Track evaluator — `anim_evaluate_track` (0x4E4370)

The heart of the system. Per-frame invariant: **evaluated parent-first,
once per frame**, gated by a global frame counter (`dword_724F38`).

Track instance fields (best-fit from decode):

| Offset | Type | Purpose |
|---|---|---|
| `+0x00..+0x3C` | float[16] | **Local matrix** working scratch (4×4) |
| `+0x40` | ptr | **Parent track ptr** (recursion target via parent->152) |
| `+0x44` | ptr | **Scene ptr** the track applies its transform to |
| `+0x48` | uint32 | **Flags** (see table below) |
| `+0x4C` | int | Last-evaluated frame counter (cache key) |
| `+0x50..+0xBC` | float[28] | **Channel state cache** (32 channels — channel 0 = visibility) |
| `+0x6C..+0x74` | float×3 | Cached translation x/y/z |
| `+0x80..+0x88` | float×4 | Cached rotation (quat) |
| `+0xC0` | int | **Keyframe count** |
| `+0xC4` | ptr | **Keyframe array** (5 dwords / 20 bytes per entry) |

Track flags (`+0x48`):

| Bit | Meaning |
|---|---|
| `0x00400000` | "has additional matrix" (combine via row-3 translate at +0x80) |
| `0x10000000` | force update (skip the "state unchanged" fast-path) |
| `0x20000000` | hold-position-on-hide (don't reset transform when channel 0 ≤ 0.5) |
| `0x40000000` | **disable evaluation** (`anim_update_all_tracks` skips this entry) |
| `0x08000000` | cascade-hidden (parent forced this hidden — set inside the evaluator) |

Keyframe entry (5 dwords / 20 bytes):

| Offset | Field |
|---|---|
| `+0x00 bits 0-15` | (curve table id / hash) |
| `+0x00 bits 16-20` | **Channel index** (0..31) |
| `+0x00 bit 21` | end-of-list marker |
| `+0x00 bit 24` | **additive mode** (`channel += value` vs `channel = value`) |
| `+0x04` | curve data ptr (-> curve segments) |
| `+0x08..+0x10` | (reserved / per-curve scratch) |

Evaluation flow (Hex-Rays summary):

```c
int anim_evaluate_track(track *this, double current_time) {
    // (1) Cache: skip if already evaluated this frame
    if (this->last_frame == g_anim_frame_counter) return;
    this->last_frame = g_anim_frame_counter;

    // (2) Recurse into parent first (so cascade flags are up-to-date)
    if (this->parent) anim_evaluate_track(this->parent->[152]);

    // (3) Hide cascade: if parent's scene is hidden, hide self too and bail
    if (this->parent && (this->parent->scene->flags & 1)) {
        this->scene->flags |= 1;        // set (scene+104) bit 0
        this->flags |= 0x08000000;
        return;
    }

    // (4) Snapshot channel state for change detection
    float v23[28]; memcpy(v23, this->channel_cache, 0x70);

    // (5) Walk keyframes; sample each curve into v23[channel]
    if (!(this->flags & 0x200000)) {  // unless "disabled" sub-flag set
        for (i = 0; i < this->kf_count; ++i) {
            kf = &this->kf_array[i*5];
            value = anim_keyframe_resolve(kf, current_time);
            channel = (kf->flags >> 16) & 0x1F;
            if (kf->flags & 0x01000000)
                v23[channel] = current_time + v23[channel];  // additive
            else
                v23[channel] = current_time;                  // absolute
        }
    }

    // (6) Channel 0 = visibility
    if (v23[0] <= 0.5f) {
        this->scene->flags |= 1;      // HIDE
        if (!(this->flags & 0x20000000))  // not "hold position"
            *(float*)this->cached_pos = 0.0f;
    } else if (parent_was_hidden && v23[0] > 0.5f) {
        this->scene->flags &= ~1;     // SHOW (recover)
        v23[0] = 1.0f;
    }

    // (7) State unchanged + force-update bit not set -> use cached transform
    if (!(this->flags & 0x10000000) && memcmp(v23, this->channel_cache, 0x70) == 0) {
        matrix4_copy(this->scene, &this->local_matrix);
        if (this->parent)
            sub_41A380(this->scene, this->parent);   // multiply parent
        return sub_4C4A20(cached_translate.x, cached_translate.y, cached_translate.z);
    }

    // (8) State changed -> commit + recompute matrix via SecuROM thunk
    this->flags &= ~0x10000000;
    memcpy(this->channel_cache, v23, 0x70);
    // ... initialize identity matrix scratch (lazily, gated by dword_726DE0) ...
    return SECUROM_THUNK(g_securom_thunk_table_base + 161538, ..., a3, a4);
}
```

Key behaviour summary:
1. **Channel 0 is visibility.** ≤ 0.5 hides; > 0.5 shows.
2. **Hide cascades through parent tree.** If a parent is hidden, every
   descendant scene is hidden (cascading via the recursive call +
   `0x08000000` flag).
3. **State change is the optimization gate.** If channels haven't moved
   since last evaluation, the matrix update is skipped (just transform
   the cached state into the scene). The 28-float `memcmp` is the
   fast-path test.
4. **Matrix recompute is SecuROM-thunked.** The actual "build TRS matrix
   from channels and apply blend" is at `g_securom_thunk_table_base +
   161538` — encrypted.

## Pool access — `anim_pool_get_at_thunk` (0x4BD730)

```c
int anim_pool_get_at(int pool_head, int idx) {
    if (!pool_head) return 0;
    return SECUROM_THUNK(g_securom_thunk_table_base + 177878,
                         *(uint16_t*)(pool_head + 2),  // count
                         pool_head, idx);
}
```

Tracks live in a SecuROM-managed slab. Globals:
- `unk_726D5C` = pool head (count word at `+2`)
- `word_7264E8` = active controller count (separate pool)

## DB-table system

Strings naming runtime database tables:

| Table | Likely contents |
|---|---|
| `DB_BONEINFO` | Bone metadata: name, parent, default pose |
| `DB_BONE_GROUP_MIRRORS` | Symmetric bone pairs (biped mirroring) |
| `DB_WORLD_ANIM_DRIVERS` | Drivers (input → curve dispatch) |
| `DB_WORLD_ANIM_CURVES` | Curve definitions (= segment arrays sampled by `anim_curve_sample`) |
| `DB_WORLD_ANIM_INSTANCES` | Per-entity playback instances (tracks) |
| `DB_WORLD_ANIM_EVENTS` | Discrete events on animations (trigger sounds, FX) |
| `DB_WORLD_ANIM_EVENT_STRING_TABLE` | Event names → IDs |
| `DB_ANIM_CURVE` | Single-curve animations (object-level, not skeletal) |

These are referenced by string in code that's SecuROM-protected — the
actual DB lookup logic is encrypted. Runtime names + instance IDs are
hashed at compile time.

## KFM — Gamebryo Key Frame Motion

`BonelessKFM` (string `0x6A891C`) is the model name token used as a
fallback for non-skinned objects. The `.kfm` format itself is the
authoring exchange format from Gamebryo.

- Public KFM specification documents the file structure (header +
  block table + animation tracks + per-bone curves). When we need to
  parse `.kfm` files on disk, the public spec is the right reference.
- `UpdateBonelessKFMInstances` is a **script command** (no static
  xrefs — registered via SecuROM-thunked `script_register_command`).
  AI scripts can trigger updates outside the per-frame walk.

## Identity-matrix scratchpad

`unk_726D60..unk_726DDC` hold an identity matrix scratch buffer. The
dword `dword_726DE0` is a bit-flag init guard: bit 0 marks the 16-float
identity initialized; bit 1 marks an alternate scratch initialized.
Both are filled lazily on first use inside `anim_evaluate_track`.

This is a micro-optimization to avoid repeatedly re-zeroing the
identity scratch when the inner SecuROM matrix builder needs an
identity input.

## What's blocked by SecuROM

| Function | Role |
|---|---|
| `anim_pool_get_at_thunk` (0x4BD730) | Track pool slab access |
| `g_securom_thunk_table_base + 161538` | TRS matrix builder (final transform compose) |
| The DB-table lookups (Driver/Curve/Instance/Event tables) | Class registry for animation graph |
| `UpdateBonelessKFMInstances` script binding | KFM file format parser entry |

For runtime work: hooking the `anim_pool_get_at_thunk` returns dumps
every active track each frame. Combined with the visible state at
`+0x40..+0xC4`, we can reconstruct the playing animation graph entirely
at runtime.

## What's left to investigate

1. **Bone hierarchy**: where are bone matrices stored on the entity?
   `mdb_parse_model` (0x5FB110) builds 80-byte node arrays — likely
   bone definitions. Bone matrices probably live on the entity at a
   per-class offset (different for `compactor` vs `digDugAnt`).
2. **Driver-curve dispatch**: how does a name like `"PlayWilburFaceAnim"`
   resolve to a specific curve set on a specific entity? Probably via
   the SecuROM-protected DB table lookup.
3. **Animation events**: the `DB_WORLD_ANIM_EVENTS` table feeds sound /
   FX triggers along the timeline. Format unclear; needs runtime trace.
4. **`.kfm` file format** in the shipped game: confirm a sample matches
   the public Gamebryo spec, write a dumper if it deviates.

## Anchors created this session

| VA | Symbol |
|---|---|
| `0x4BD730` | `anim_pool_get_at_thunk` (SecuROM) |
| `0x4E2B00` | `anim_controller_advance` |
| `0x4E2D80` | `anim_curve_sample` (cubic Bezier + lerp + step) |
| `0x4E4220` | `anim_keyframe_resolve` |
| `0x4E2CA0` | `anim_advance_time` (already named) |
| `0x4E4370` | `anim_evaluate_track` (already named) |
| `0x4E4B70` | `anim_update_all_tracks` (already named) |

## See also

- [`systems-survey-2026-05-06.md`](systems-survey-2026-05-06.md) — 8-stage simulation tick (anim is stage 7)
- [`level-loading.md`](level-loading.md) — `.dbl`/`.mdb`/`.kfm` asset zoo
- [`entity-system.md`](entity-system.md) — `mdb_parse_model` + entity vis cascade (also touches `(scene+104) bit 0`)
- [`frame-pacing.md`](frame-pacing.md) — physics-vs-FPS caveat applies to anim too (0.003 step)

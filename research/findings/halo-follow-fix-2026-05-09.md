# Halo follow-fix — RE + implementation (2026-05-09)

> **Bug:** with mtr-asi's M3 view-interp ON, halo sprites (mission-objective
> tags, NPC follow markers, etc.) appeared visibly offset from the entity
> they tagged, with the offset modulating per-frame as the camera rotated.
>
> **Status:** RE complete, fix shipped. `halo_interp` toggle in
> tab_performance, default ON in the Recommended preset.
> `Game/mtr-asi.asi` = 553472 bytes.

---

## 1. Engine surface area

The HaloComponent owns all in-world halo / marker sprites. Two storage tiers:

| Tier | Storage | Add path | Field |
|------|---------|----------|-------|
| **Static specs** | 180-byte struct array | `halo_load_specs_from_dbl` (`0x6667F0`, called once per scene load via `flares\halos.dbl`) | `HaloComponent + 0x1C` (ptr), `+0x20` (count) |
| **Dynamic halos** | 204-byte structs in a doubly-linked list | `halo_dynamic_spawn_init` (`0x666FF0`) → `halo_dynamic_alloc_node` (`0x666CD0`) | head at `HaloComponent + 0x24`, tail `+0x28`, count `+0x30`; nodes link via `+196 = prev`, `+200 = next` |

The HaloComponent itself is **52 bytes**, allocated by `halo_component_ctor`
(`0x666F90`). Vtable is at `0x6DD400`. The singleton pointer lives at
`g_scene_cvar_block + 0x84C` (`= 0x7459A4`).

Class hierarchy clue: vtable slot 7 (`0x5E38B0`) returns the literal
`"HaloComponent"`. There is also a slot 2 (`0x5E31A0`) on a sibling vtable
returning `"IRRControl"` — likely the parent class.

### Per-frame entry point

The component is registered into a global scene-component linked list
(head at `[0x745A84]`) by `scene_component_list_register` (`0x672CA0`),
called from `halo_component_ctor`.

Each render frame, **`engine_pump_alt`** (`0x682010`) walks this list and
calls `vtable[+4]` (the Update method) on each node:

```
engine_pump_alt:
   timer_wheel_pretick
   sub_5AD4D0
   engine_pump_alt_pre_sim
   alt_pump_subsystem_sweep
   ━━━━━━ HALO BLOCK at 0x68149A ━━━━━━
   if ([0x745A84] != null):
      camera = world_ctx_get_camera_struct(world_ctx, 0)
      fov, aspect, near, far = camera->vtable[3..5]()
      halo_stash_camera_proj_params(fov, aspect, near, far)   ; writes proj -> 0x745AA0
      scene_component_list_dispatch_update([0x745A84], 0)     ; walks list -> halo_component_update (and any other registered components)
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   wave_grid_tick(0.003)
   simulation_tick_aggregator
   render_frame_top_level
      ...
      camera_apply_all_active   ← M3 view-interp lerps 0x724C10 here
      ...
      [render meshes with interp'd view]
```

`halo_component_update` (`0x6678D0`) is the per-frame Update method. It:

1. Fetches the camera struct via `world_ctx_get_camera_struct(dword_741648, 0)`.
2. Reads `camera_struct + 0x150` → vec3 = camera world pos (used for halo
   audio listener via `sub_6734E0`).
3. Iterates static specs (180-byte stride), calling
   `halo_spec_project_and_submit(this, camera_struct, threshold², spec)`.
4. Iterates dynamic linked list, calling
   `halo_dynamic_project_and_submit(camera_struct, threshold², halo)`.

### How halo screen positions are projected

Both `halo_spec_project_and_submit` and `halo_dynamic_project_and_submit`
call `camera_project_world_to_screen` (`0x58B6F0`) — or the aspect-aware
variant `0x58B7D0`. The projection happens via:

```c
// camera_project_world_to_screen(this=camera_struct, world_pos, &screen_xy_out, &depth_out):
sub_4B4720(world_pos, &clip4, this + 0x110);   // clip4 = world_pos * matrix_at_+0x110
if (clip4.z < 0 || clip4.z > clip4.w)  return reject;
if (|clip4.x| > clip4.w || |clip4.y| > clip4.w)  return reject;
screen.x = (clip4.x / clip4.w + 1.0) * 0.5;
screen.y = (clip4.y / clip4.w + 1.0) * 0.5;
```

So **`camera_struct + 0x110`** is the cached **view-projection matrix**
(row-major, 16 floats). This is the matrix that determines where each
halo lands on screen.

The screen-space XY then feeds `halo_build_screen_quad_corners`
(`0x672FE0`) which writes 4 quad corners (12 floats: pos+uv) into the
halo's sprite vertex buffer — the renderer picks these up later.

---

## 2. Why the bug was visible only with view-interp

| Frame stage | What | Reads / writes |
|-------------|------|----------------|
| `engine_pump_alt`, halo block | `halo_component_update` projects every halo to screen | reads `camera_struct + 0x110` (cached VP) |
| `simulation_tick_aggregator` | sim updates entity transforms | — |
| `render_frame_top_level → camera_apply_all_active` | M3 view-interp lerps global view | writes `0x724C10` (interp'd V) |
| `render_frame_top_level → render meshes` | renderer transforms vertices | reads `0x724C10` and `0x745AA0` |

Without view-interp, both pipelines use the same `V`. With view-interp,
the geometry renderer uses **`V_interp`** while the halo update
already-cached its screen projection using **`V_curr`** (from
`camera_struct + 0x110`, which was built before halo_component_update
fired). The same world position projects to two different screen pixels
under the two different V's → halo offset that scales with the per-
frame interp delta.

The user originally observed this as "Wilbur's mom highlight follows
with a ghost-offset that varies as I rotate the camera" (2026-05-08).

---

## 3. The fix (M3.3)

Hook `halo_component_update` PRE-orig. While the hook fires:

1. Bail out fast if any of: feature off, view-interp off, no two
   snapshots, cut detected, sim not throttling, camera struct
   unresolvable.
2. Resolve camera_struct via `world_ctx_get_camera_struct(*dword_741648, 0)`
   (replicates engine's call site).
3. Save `camera_struct[+0x110]` (16 floats = 64 bytes).
4. Build `V_interp` from M3 snapshots — same slerp+lerp as
   `view_apply_interp_for_render_frame`.
5. Compute `VP_interp = V_interp · V_curr_inv · VP_curr`. The diff
   approach preserves whatever engine-specific adjustments are baked
   into `VP_curr` (clip-plane skew, viewport remap, etc.) — we only
   propagate the V change.
6. Write `VP_interp` into `camera_struct[+0x110]`.
7. Call orig (which projects all halos through the interp'd VP).
8. Restore `camera_struct[+0x110]`.

The block is SEH-wrapped — if the camera struct is being torn down
mid-call (level transition), best-effort restore + return, no crash.

`camera_struct + 0x150` (audio listener vec3) and `+0x168` (view-forward
vec3) are NOT overridden — the audio attenuation is sub-frame and the
backface-cull edge cases are below user-perceptible noise. Keeping the
hook surface area minimum-viable per RULE №1.

### Math reminder

D3D row-major, `v_clip = v_world · V · P = v_world · VP`.

We want `VP_interp = V_interp · P`. We only have `VP_curr = V_curr · P`,
so `P = V_curr_inv · VP_curr`, which gives:

```
VP_interp = V_interp · V_curr_inv · VP_curr
```

For a row-major D3D view matrix `V = T(-C) · R^T`, the inverse has top-3x3
= transpose of V's top-3x3, translation row = camera world position C
(via `extract_camera_world_pos`), right column = (0,0,0,1).

### Why hook here and not at a higher level

Other options considered:

- **Lerp `camera_struct + 0x110` earlier in the frame** (e.g. PRE
  `engine_pump_alt`'s halo block): would require finding when/where the
  engine builds the cached VP each frame, and the side-effects across
  every other system that reads VP would need auditing.
- **Hook `camera_project_world_to_screen` (`0x58B6F0`) globally**: works,
  but affects 5+ unrelated callers (some hit-test, some particle
  culling). Risk of regressions.
- **Hook the per-halo projector (`halo_spec_project_and_submit` /
  `halo_dynamic_project_and_submit`)**: per-halo overhead, repeated
  save-write-restore — same effect as one hook on the parent, just more
  expensive.

Hooking `halo_component_update` is the smallest blast radius that fixes
the bug. The override fires once per render frame regardless of halo
count.

---

## 4. UI surface

`Tools → Performance → Smooth camera between sim ticks → Fix marker / halo offset`.

Default OFF, but the **Recommended** decouple preset now turns ON
`view_interp + halo_interp` together. Both `Throttle only` and `Off`
presets keep them off.

UI shows two diagnostic counters:

- **Halo overrides:** cumulative count of frames where the VP override
  fired.
- **Skips:** cumulative count of frames where the hook bypassed the
  override (feature off, no snapshots, cut, mini-game vetoed, camera
  unresolved).

In a healthy session with `halo_interp` ON during normal gameplay,
overrides should grow at the render-frame rate; skips should stay near
zero except during scene cuts and level loads.

---

## 5. Renames + comments landed in IDB

| Address | New name | Was |
|---------|----------|-----|
| `0x6678D0` | `halo_component_update` | sub_6678D0 |
| `0x666F90` | `halo_component_ctor` | sub_666F90 |
| `0x666AB0` | `halo_component_dtor` | sub_666AB0 |
| `0x672BE0` | `scene_component_list_dispatch_update` | sub_672BE0 |
| `0x672CA0` | `scene_component_list_register` | sub_672CA0 |
| `0x672C70` | `scene_component_list_alloc` | sub_672C70 |
| `0x672CE0` | `scene_component_list_free` | sub_672CE0 |
| `0x58DA10` | `world_ctx_get_camera_struct` | sub_58DA10 |
| `0x672DB0` | `halo_stash_camera_proj_params` | sub_672DB0 |
| `0x672D10` | `matrix4_make_perspective_d3d` | sub_672D10 |
| `0x58B6F0` | `camera_project_world_to_screen` | sub_58B6F0 |
| `0x58B7D0` | `camera_project_world_to_screen_aspect` | sub_58B7D0 |
| `0x672FE0` | `halo_build_screen_quad_corners` | sub_672FE0 |
| `0x666CD0` | `halo_dynamic_alloc_node` | sub_666CD0 |
| `0x666A50` | `halo_dynamic_unlink_node` | sub_666A50 |
| `0x666B00` | `halo_set_hidden_in_aabb` | sub_666B00 |
| `0x666B90` | `halo_compute_world_pos` | sub_666B90 |
| `0x6670F0` | `halo_spec_project_and_submit` | sub_6670F0 |
| `0x6675A0` | `halo_dynamic_project_and_submit` | sub_6675A0 |
| `0x5E38B0` | `halo_get_class_name` | sub_5E38B0 |
| `0x5E3960` | `halo_register_spec_via_script` | sub_5E3960 |
| `0x6667F0` | `halo_load_specs_from_dbl` | sub_6667F0 |
| `0x666FF0` | `halo_dynamic_spawn_init` | sub_666FF0 |
| `0x673790` | `halo_set_world_pos` | sub_673790 |

Comments at the camera-struct offset reads (`+0x110`, `+0x150`, `+0x20`,
`+0x24`) and at the dispatch site in `engine_pump_alt`.

---

## 6. Open follow-ups

- Pending **runtime confirmation** that the fix lands correctly under M3
  view-interp ON. Build deployed at 553472 bytes.
- The "All smoothing" preset bundles halo_interp with the unverified
  Wilbur (M4) and NPC (M5) interp paths. If the user reports halo_interp
  works but Wilbur looks janky, the M4 fence assumption is the next
  thing to dig into.
- The audio listener position at `camera_struct + 0x150` doesn't get
  the interp treatment. If users report any audible "thump" on halo
  noise sounds when fast-rotating with view-interp ON, we'd need to
  also override `+0x150` in the same fence.

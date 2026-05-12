# Comprehensive decoupling audit — every per-tick subsystem

Date: 2026-05-07 (Phase 2+ ongoing)
Status: in progress, multi-week scope.

This document catalogs every subsystem in the Wilbur engine that
integrates with a timestep, tracks how each gets its dt, and records
the fix status under our dt_correctness module.

## Background

The user's testing revealed that Phase 1 dt-correctness fixed the
camera but NOT particles — particles continued running at 240 Hz
visually even at sim_hz=15. UI tweens DID slow with sim_hz. This
asymmetry exposed that:

1. Different subsystems use different dt sources.
2. Some take dt as a **function parameter** (not a global), so
   patching `flt_6FFCBC` doesn't reach them.
3. There are multiple timing globals (`flt_6FFCBC`, `dword_6FFCA4`,
   `flt_6FFCC4`, particle context dword_724DFC[0/1], etc.).

Goal: every per-tick subsystem must integrate at the user's chosen
time scale (RealTime / SlowMo / Off) consistently.

## Timing global catalog

| Global | VA | Type | Static value | Readers | Writers (engine) | Our hooks |
|---|---|---|---|---|---|---|
| `flt_6FFCBC` (g_engine_universal_dt_003) | 0x6FFCBC | float | 0.003 | **153** | None (init at 0x6A3F60 unused) | dt_correctness writes from camera_apply + sim_aggregator hooks |
| `dword_6FFCA4` (kFrameDtMsGlobalVA) | 0x6FFCA4 | uint32 (ms) | 3 | 33 | One unused initializer (0x6A3F60) | dt_correctness writes ms from sim + render hooks (Phase 2) |
| `flt_6FFCC4` (purpose unknown) | 0x6FFCC4 | float | ~0.95 | **108** | One unused initializer (0x6A3F99) | None — needs investigation |
| `dword_724DFC` | 0x724DFC | ptr→struct | (init by sub_67F010 to allocated 2992-byte particle context) | particle_update_tick | sub_67F010, sub_570190 cleanup | particle_buckets_sweep_a writes context[0]/[1] = flt_6FFCBC / dword_6FFCA4 |
| `dword_72633C` | 0x72633C | ptr→struct | (sister of 724DFC for sweep_b) | sub_4D5170 | sub_67F010-style init | particle_buckets_sweep_b writes context[0]/[1] |
| `unk_700640[0]` | 0x700640 | float | 0 | frame_dt_ring_update writes (= elapsed_ms × 0.066666) | frame_dt_ring_update | None |
| `unk_741330[0..14]` | 0x741330 | uint32[15] | (ring of 15 timestamps) | frame_dt_ring_update | frame_dt_ring_update | None |
| `dword_6FFCAC` | 0x6FFCAC | uint32 (timestamp) | (initial 0 or per-init) | unknown | (init at 0x6A3F65 only) | None |
| `dword_6FFCB0` | 0x6FFCB0 | uint32 | 0 | unknown | (init only) | None |
| `dword_6FFCB8` | 0x6FFCB8 | uint32 | 0 | unknown | (init only) | None |
| `dword_6FFCC8` | 0x6FFCC8 | uint32 (or float) | 0x3C8B4396 (≈0.0167) | sub_4F75D0 (1 ref) | (init only) | None |

### Investigation: `flt_6FFCC4`

108 readers. Static value 0.95. Single writer is the unused
initializer that sets it to 0. Since initializer is unused, the runtime
value is 0.95.

Sample reader (sub_4F1160 line 0x4f146f):
```c
*((_DWORD *)this + 54) = flt_6FFCC4;  // = 0.95
```

The function uses this+54 as a "frame elapsed time" counter, decremented
each call. So flt_6FFCC4 isn't a per-frame dt — it's a CONSTANT (= 0.95)
used as a "default frame time".

Conclusion: flt_6FFCC4 is a **constant**, not a timestep. Don't patch.

## Subsystem catalog (sorted by call cadence)

### Sim path (in simulation_tick_aggregator)

These run ONCE per sim_aggregator orig invocation. Under Phase 1+2, sim
fires at user's target_hz with our dt_correctness writing flt_6FFCBC =
real_sim_dt × time_scale to scale these correctly.

| Function | VA | dt source | Status |
|---|---|---|---|
| `frame_dt_ring_update` | 0x584780 | `j_game_get_time_ms` (independent) | ✓ unaffected — uses real time |
| `entity_transform_tick` | 0x4B9F60 | `flt_6FFCBC` | ✓ dt-correct |
| `physics_state_machine_tick` | 0x4DC150 | `flt_6FFCBC` | ✓ dt-correct |
| `trail_subsystem_tick` | 0x4D1D60 | `flt_6FFCBC` | ✓ dt-correct |
| `particle_buckets_sweep_a` | 0x4BAA40 | reads `flt_6FFCBC` + `dword_6FFCA4`, writes to particle context | ✓ dt-correct (sim path) |
| `particle_buckets_sweep_b` | 0x4D3E50 | sister of sweep_a | ✓ dt-correct (sim path) |
| `anim_update_all_tracks` | 0x4E4B70 | calls anim_advance_time → anim_controller_advance | ✓ dt-correct (per-track uses flt_6FFCBC) |
| `sub_60EED0` (sim subscriber list) | (SecuROM) | passed `dword_6FFCA4 * 0.001` | ✓ dt-correct (Phase 2 dword update) |

### Render path (after camera_apply_all_active hook)

These run ONCE per render frame. dt_correctness writes flt_6FFCBC =
real_render_dt × time_scale at top of camera_apply, so all of these see
correct dt for their call rate.

| Function | VA | dt source | Status |
|---|---|---|---|
| `pathcam_smooth_pretick` | 0x58C000 | `flt_6FFCBC` | ✓ dt-correct |
| `tick_2d_overlay_pass` | 0x4A9F10 | `flt_6FFCBC` OR `dword_6FFCA4 × 0.001` (per `unk_71D2A0` flag) | ✓ dt-correct (Phase 2 dword fix) |
| `render_uv_scroll_tick` | 0x4C24E0 | `flt_6FFCBC` | ✓ dt-correct |
| `render_screen_shake_tick` | 0x4D1FB0 | computes real_dt then **discards** for `flt_6FFCBC` | ✓ dt-correct (uses our patched value) |
| `render_per_context_depth_pass` | 0x4BC890 | reads `g_d3d_view_matrix_global` (no dt) | n/a |
| `render_sprite_batcher` | 0x4E8D30 | walks unk_7271E8 (no dt) | n/a |

### Alt-pump path (in engine_pump_alt sub_682010)

Run ONCE per alt-pump iteration. Currently dt_correctness doesn't touch
alt-pump explicitly, but flt_6FFCBC is whatever the most recent
sim/render hook wrote — usually render_dt by the time alt-pump fires.

| Function | VA | dt source | Status |
|---|---|---|---|
| `wave_grid_tick` | 0x4B15C0 | `flt_6FFCBC` | partial — gets last-written value (render_dt usually) |
| `chain_physics_tick_pass` | 0x4AE300 | `flt_6FFCBC` | partial |
| `managed_object_list_tick` | 0x4B3BC0 | `flt_6FFCBC` | partial |
| `timer_wheel_pretick` | 0x681380 | `flt_6FFCBC` | partial |
| `post_render_entity_sweep` | 0x596610 | `flt_6FFCBC` | partial |
| `alt_pump_subsystem_sweep` | 0x602F10 | `flt_6FFCBC` | partial |
| `alt_pump_pre_sim_audio_sweep` | 0x6582F0 | `flt_6FFCBC` | partial |

**TODO**: add an alt_pump entry hook so flt_6FFCBC gets a per-alt-pump-iteration
dt.

### Parameter-dt path (vtable-dispatched, dt as function arg)

These take `dt` as a `float` argument. Caller decides what dt is.
Phase 2 hooks `sub_4F45F0` to scale dt by time_scale; other functions
in this category not yet hooked.

| Function | VA | dt source | Status |
|---|---|---|---|
| `sub_4F45F0` (particle integrator) | 0x4F45F0 | param `a2` (caller-supplied) | ✓ Phase 2 hook scales dt |
| `sub_47E6B0` (per-particle update) | 0x47E6B0 | param `a2`, calls sub_4F45F0(this, a2) | partial — sub_4F45F0 hook covers downstream effect |
| `sub_47A060` (some object spring-step) | 0x47A060 | param `a2` → spring_step_3d_pseudo_exp(..., a2, rate) | not hooked |
| `sub_47CF60` (some object update) | 0x47CF60 | param `a2` → spring_step_3d_pseudo_exp(..., a2, rate) | not hooked |
| `sub_4F1160` (3.384-sec countdown) | 0x4F1160 | computes `v24 = 3.384 - this+54` → spring(..., v24, 0.033) | not hooked (uses self-timing) |
| `sub_406330` (state-machine spring) | 0x406330 | passes `*(this+40)` (object field) | not hooked (object-internal dt) |
| `sub_5EF4D0` (magnet/attraction) | 0x5EF4D0 | uses `flt_6FFCBC=0.003` directly | ✓ dt-correct via global |
| `sub_5F0520` (similar to 5EF4D0) | 0x5F0520 | similar | ✓ dt-correct |

### Spring step callers (sub_405630 family)

`spring_step_3d_pseudo_exp` (renamed from sub_405630) has 10 callers.
The dt arg is the second-to-last:

| Caller | dt passed | Notes |
|---|---|---|
| `pathcam_smooth_pretick` (2x) | `0.003` (= flt_6FFCBC) | ✓ dt-correct |
| `sub_406330` (2x) | `*(this+40)` (object field) | object-internal — needs case-by-case investigation |
| `sub_47A060` | param `a2` | needs hook if relevant |
| `sub_47CF60` (2x) | param `a2` | needs hook if relevant |
| `sub_4F1160` | `v24 = 3.384 - this+54` | self-timing (countdown) |
| `sub_5EF4D0` | `0.003` | ✓ dt-correct |
| `sub_5F0520` | `0.003` (likely) | ✓ dt-correct |

### Player and entity-transform-list path

Walked by entity_transform_tick (sim path) AND by NPC interp (render path).

- entity transforms (entity+0x58 pos, entity+0x70 rot 3x3) written at
  sim time by entity_transform_tick.
- M4 player_interp / M5 npc_interp write interp values at render time.
- Fence assumption: nothing else reads/writes between camera_apply-POST
  and next sim-PRE.

**Open question**: case 1 (matrix cache at entity+0x10) vs case 8 (raw
pos/rot at entity+0x58/+0x70). Wilbur's specific case unknown without
runtime data. Phase 1.5 fence-violation diagnostic will surface this.

## Outstanding investigations (this session continues)

### Mesh skinning + bone evaluation timing

Initial investigation (Phase 3): Actor class vtable at 0x6CBE68.
- vtable[24] = 0x59AAF0 — IDA's symbol resolution is broken here
  (claims `sub_693E4D` but address is way off). Likely a stub or
  cross-section reference. Needs deeper investigation.
- vtable[46] = sub_5B19B0 — just writes 0x7F7F0906 (max float) to
  entity+0x64..+0x6F (12 bytes). NOT a bone update; more like a
  reset or init sentinel.

**Tentative conclusion**: Wilbur is probably **case 1** in
entity_transform_tick (matrix cache at entity+0x10), NOT case 8 (raw
pos/rot). Case 1 builds entity+0x10 = 4x4 world matrix from
node_local + entity transform. Mesh draws read entity+0x10 directly.

**M4 implication**: if Wilbur is case 1, our player_interp writes to
entity+0x58/+0x70 don't reach the renderer (which reads entity+0x10).
Phase 5 candidate: **also write the interp matrix to entity+0x10** so
case-1 mesh draws see interp position.

To do this we'd need to:
1. Compose 4x4 world matrix from interp_pos + interp_rot (slerp/lerp).
2. Write to entity+0x10.
3. Possibly call sub_4C4960(entity+0x10) to register the updated bbox.

The fence-violation diagnostic (Phase 1.5) tracks entity+0x58 only,
so it WILL miss case-1 violations where the mesh reads entity+0x10
instead. Needs separate diagnostic.

### AI / script VM timestep

- Per project_ai_script_vm.md: AI is event-driven, not per-frame.
- But scripts can have timer-based logic. What dt do those use?
- ControlMapper input polling — is it dt-driven?

### D3D state queue / matrix stack

- When are world matrices submitted per-mesh? `wrap_SetTransform` calls
  in sub_4D7AE0 / render_per_context_depth_pass / game_render_main_scene.
- Per-particle world matrices? Probably handled in render_sprite_batcher
  or sub_4D5170.

### Stress-test edge cases

- sim_hz=1 (extreme low rate): does physics blow up? Animation skip?
- render rate = 480, 1000 (extreme high): any subsystems break?
- sim_hz > render_hz (sim faster than render): what happens to
  accumulator?

## Implementation status summary

Phase 1 (shipped):
- `dt_correctness::write_for_render_frame()` — flt_6FFCBC = render_dt
- `dt_correctness::write_for_sim_run()` — flt_6FFCBC = sim_dt
- All 153 flt_6FFCBC consumers via global write
- Pathcam unthrottled, accumulator pattern

Phase 1.5 (shipped):
- Player handle staleness + fence-violation diagnostic
- Camera-world-space view interp

Phase 2 (shipped this session):
- HUD staleness fix: dword_6FFCA4 written from render path too
- Particle integrator hook (sub_4F45F0) with dt scaling
- TimeScale enum: RealTime / SlowMoAtLowSim / Off
- Time-scale UI control

Phase 3 (planned):
- Hook alt_pump entry to write flt_6FFCBC per alt-pump iteration
- Hook remaining parameter-dt functions (sub_47A060, sub_47CF60)
- Investigate mesh skinning timing
- Investigate `flt_6FFCC4` (turned out to be a constant, not a timer)
- Stress-test extreme sim_hz / render rates

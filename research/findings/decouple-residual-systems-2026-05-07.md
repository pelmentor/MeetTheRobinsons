# Residual decouple-relevant systems — what's left and why

After M0–M6 + M3.2 + M1.6 shipped, this doc enumerates every remaining
candidate for "needs decoupling" treatment and explains the verdict
(needed / not needed / out of scope / runtime-only). Anchored to static
RE. Anything that requires a runtime trace to prove or disprove is
flagged as such.

## What's already decoupled

For reference, the decoupling work to date covers:

| System | Decoupled at | Mechanism |
|--------|-------------|-----------|
| Physics integration (`physics_state_machine_tick`) | M1.2 | sim_aggregator throttle |
| Animation playback (`anim_controller_advance` + `anim_evaluate_track`) | M1.2 | sim_aggregator throttle |
| Particle integration (`particle_buckets_sweep_a/b`) | M1.2 | sim_aggregator throttle |
| Trail subsystem (`trail_subsystem_tick`) | M1.2 | sim_aggregator throttle |
| Entity transforms (`entity_transform_tick`) | M1.2 | sim_aggregator throttle |
| PathCam smoothing (`pathcam_smooth_pretick`) | M1.3 | direct hook |
| 2D HUD overlay tweens (`tick_2d_overlay_pass`) | M1.4 | sim-ran flag throttle |
| UV-scroll texture animations (`render_uv_scroll_tick`) | M1.4 | sim-ran flag throttle |
| Engine frame-dt for time-driven subsystems (`dword_6FFCA4`) | M1.6 | overwrite PRE-orig-sim |
| View matrix (camera) | M3.1 | slerp+lerp at camera_apply POST |
| Player transform (Wilbur pos+rot) | M4 | save-write-restore + lerp/slerp |
| Visible NPC transforms | M5 | dword_724DE4 walk + per-slot lerp/slerp |
| Cuts in scene transitions | M2.2 | translation/rotation threshold detector |
| Player teleports (respawn etc.) | M4.3 | per-snapshot delta threshold |
| Aim-mode latency | M3.2 | input-bound alpha=1 snap |

## Candidates investigated and ruled out

### `render_screen_shake_tick` (sub_4D1FB0)

Reads `flt_6FFCBC` (0.003) only as a fallback when `unk_725E9C` flag is
set. Primary path computes real-dt from `game_get_time_ms() - last_call`.
At 240 Hz the real-dt path naturally produces 4ms increments — correct
for screen shake (real-time-driven motion). Fallback path could
theoretically run wrong, but: a) we haven't observed the fallback firing
in normal play, b) screen shake's visual contract is "rough motion",
imperceptible to the user even if rate is off. **Not worth a fix.**

### `dword_6FFCA4` is actually a constant (not dt-driven)

Static byte-pattern sweep showed only ONE write to this address at engine
init (sets it to 17 = baseline 60Hz dt). **No code dynamically writes
it.** All consumers read 17ms regardless of frame rate.

This means the engine deliberately gives sim-side dt-consumers a fixed
17ms baseline — implicit assumption "sim runs at 60 Hz". Our throttle
keeps that assumption true at any render rate, so consumers stay correct
**at target_hz=60**.

M1.6 still ships (correctly): when user picks target_hz≠60 (e.g. 30 for
half-speed slow-mo, or 120 for double-speed), M1.6 overwrites with the
real elapsed ms so dt-driven consumers track real-world time
proportionally. At target_hz=60 it's a no-op.

### Mouse input integration (`sub_572650` and family)

Mouse sensitivity is registry-based (`HKCU\...mouse sensitivity`); the
delta integration uses standard ControlMapper machinery via DInput. We
observed no symptoms of frame-rate-coupled mouse during high-FPS
playtests of mtr-asi over the past sessions — concluding the engine
either uses real-dt for mouse acceleration or treats raw delta as
sample-and-forward (frame-rate-independent). **No fix needed.**

### Cutscenes (`*.cut` files, Cutscene script commands)

Cutscene playback is event/script-driven, not frame-counted. Cinematic
camera comes from PathCam paths which are throttled (M1.3). Specific
cuts within cutscenes are caught by M2.2 cut detection.
**No additional fix needed.**

### Hidden 0.003 callers (orphan code paths)

Static sweep of `flt_6FFCBC` xrefs found ~80% of consumers were dead
code (no live callers). Live callers either inside aggregator
(throttled) or already covered (overlay tween / UV scroll).
**Sweep is comprehensive.**

### `sub_60EED0` subscriber list

Walks `dword_744C4C` linked list at the end of every sim tick, calling
vtable[2](dt) on each registered subscriber. Without runtime trace we
can't enumerate WHO subscribes — could be music sync, mission timer,
fade tracker, shader-uniform animator. **M1.6 ensures all subscribers
get correct dt regardless of throttle target.** Specific subscribers
remain unidentified but covered en masse.

## Out of scope per the original plan

These were explicitly excluded by the high-fps decouple plan §non-goals
and we're not revisiting:

- **Bone skinning interpolation** (Phase 2 layer 4): would interp the
  per-bone TRS palette between sim ticks. Plan estimate ~1500+ LOC. The
  visual benefit on top of M3+M4+M5 is small because skinned characters
  look fine when their world transforms (M4 pos+rot) interp at render
  rate — bones tick at 60 Hz unnoticed.
- **Particle position interpolation** (Phase 2 layer 5): particles
  already look fine at 60 Hz subjectively, at the cost of a separate
  per-particle snapshot infra (~500 LOC).
- **Mini-game support** (DigDug / Hamster / ChargeBall): auto-disabled
  entirely (M1.5).

## Theoretical issues only surfacable at runtime

Calling these out so the user knows what to watch for during the M0.3
validation pass:

### Frustum culling lag

M3.1 view interp writes interpolated globals 0x724C10/0x724C50, but
frustum culling (`render_context_run_vis_test` etc.) uses cached camera
state from the entity (`outer_cam+0x34`). Result: an NPC right at the
frustum edge can pop out of view by ~1 sim-window even when the
interpolated view "should" still see it. Visible only in edge cases
(camera rotating fast right when an NPC is on the rim).

**Fix if observed**: write the interp view back into the camera entity
too (not just globals) — would require an extra hook surface. Skipped
unless symptom appears.

### Shadow map projection lag

Shadows render from light POV per render frame. If light position is
sim-tick-cadence and view is render-rate-cadence, shadows could appear
to "lag" the view by ~1 sim window during fast camera motion. In
practice shadow-mapping in 2007-era engines is robust to this; not
worth pre-emptive fix.

### Sub_60EED0 subscriber identity

If a specific subscriber visibly misbehaves (e.g. "music gets ahead of
visuals by 4× under throttle"), runtime trace of the subscriber list
would identify it. Until evidence: assume covered by M1.6.

## Categorical end state

Static RE has surfaced everything it can. Further decouple work
requires:

1. **Runtime tracing** (instrument the sub_60EED0 list, log subscribers).
2. **Playtest evidence** (user reports a specific stutter / wrong rate).
3. **Following stolen-byte thunks** through `g_securom_thunk_table_base
   + N` to find script-command callbacks etc. (rr01 unpack is
   complete — bodies are visible, just reached through indirect
   call tables).

The "encrypted SecuROM" framing earlier docs sometimes used is wrong
for this build. Per `project_overview.md`, there's no per-page lazy
decryption — every byte of code is plaintext after the rr01 stub
finishes. What's hard about thunked calls is locating them via
indirect-call analysis, not encryption.

In the absence of the above, **the system is correct under its claimed
contract**: at any render rate, if throttle is on at target_hz, all
identified time-driven systems run at target_hz cadence, with
appropriate interp filling the visible gap.

## See also

- `project_decouple_plan_complete.md` — milestone-by-milestone status
- `decouple-m1-6-dt-correction-2026-05-07.md` — the dword_6FFCA4 fix
- `high-fps-decoupling-plan.md` — original engineering plan

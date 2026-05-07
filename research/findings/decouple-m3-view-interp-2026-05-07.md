# M3.1 — view interp shipped

The first decouple milestone with a **visible** payoff. With M3.1 enabled at
240 Hz render + 60 Hz throttled sim, the game's camera moves at the
render rate (smooth, no per-sim-tick stutter) while everything else
ticks at 60 Hz like the engine intended.

## Architecture

Builds directly on M2's snapshot infrastructure:

```
hk_camera_apply_all_active POST:
  // (M2) snapshot the freshest globals into curr (shifting curr -> prev)
  // (M2) cut detection: if curr-vs-prev delta > thresholds, flag this frame as a cut

  // (M3.1) write interpolated matrices back to globals:
  if (view_interp_enabled
      && has_two_snapshots
      && !cut_for_curr
      && sim_decouple::effective_is_throttling())
  {
      alpha = current_alpha()                       // 0..1, clamped
      out_view  = compose_interp(prev.view,  curr.view,  alpha)
      out_world = compose_interp(prev.world, curr.world, alpha)
      memcpy globals 0x724C10 = out_view
      memcpy globals 0x724C50 = out_world
  }
```

`compose_interp_matrix` is slerp on the rotation 3x3 + lerp on translation
row 3:

1. Convert prev/curr's 3x3 rotation block to a quaternion via
   Shepperd's method (handles all sign cases without divide-by-near-zero).
2. `quat_slerp(q0, q1, alpha)` with shortest-path correction (negate q1
   if `dot(q0, q1) < 0`) and nlerp fallback for `dot > 0.9995` (avoids
   sin(small) instability).
3. Convert slerp result back to a 3x3 rotation.
4. Lerp translation `[12], [13], [14]` linearly.
5. Pass through `[3], [7], [11], [15]` from curr (typically `[0,0,0,1]`).

## Why no PRE/POST fence (M2.3)

The plan §M2.3 describes save-write-restore so writes don't pollute the
sim's reads. But the sim never reads globals 0x724C10/0x724C50 — those
are render-only consumers (per the static xref sweep in
[`decouple-m2-snapshot-infra-2026-05-07.md`](decouple-m2-snapshot-infra-2026-05-07.md)).

The next render frame's `camera_apply_all_active` overwrites globals
unconditionally. So the lifecycle is:

```
frame N:  camera_apply -> globals = curr_N      -> our hook writes interp_N -> render reads interp_N
frame N+1: camera_apply -> globals = curr_N+1   -> our hook writes interp_N+1
```

No state leaks across frames. No fence needed at this milestone.

When M4 lands and writes per-entity transforms (Wilbur pos/rot
overrides on the player entity), THOSE writes will need a fence — sim
reads its own entity state. That's the fence work; it's bound to M4,
not M3.

## Throttle gate

View interp only fires when `sim_decouple::effective_is_throttling() ==
true` — i.e. mode == THROTTLE AND mini-game auto-disable hasn't vetoed
it. When throttle is off, render and sim run lock-step (1:1) and there's
no inter-tick gap to fill, so interp would just lerp(prev, curr=prev, ?)
identity. Skipping the work is the correct no-op.

## Toggles

- **Tools → Decouple → "Enable view interp (M3.1) — slerp+lerp per render frame"**
  - Default OFF. User opts in.
  - Wired to FPS overlay's `CAM:ON`/`CAM:off` flag.
- **Cut-detect translation/rotation thresholds** — already shipped in
  M2. Tune to taste; a too-loose translation threshold means cuts get
  missed and the cinematic slides; too-tight means walking around
  triggers false cuts.

## Inherent latency

Lerp shows the state interpolated **between** consecutive sim ticks, so
the rendered view is always ~1 sim-window behind the freshest sim. At
60 Hz target that's 16.7 ms vs uncapped render. This is the price of
correct (non-extrapolating) interpolation per D4 — the alternative
(predict the next tick from velocity) jitters on direction changes and
introduces artifact classes that can't be cleanly fixed.

For aim mode the latency is perceptually worse because the user expects
"my crosshair tracks the stick *now*". M3.2 (aim-mode snap) will set
`alpha = 1.0` for aim frames so they show the freshest sim immediately.
That's a separate RE — needs to detect when `SetPathCamTargetingBehavior`
script command is in effect.

## Counter

`view_interp_writes()` cumulative = how many render frames the M3.1 path
fired. Useful for "is interp actually engaging?" sanity check. At 240 Hz
with throttle@60 + view interp on, expect ~240 / sec.

## Performance

- `mat3x3_to_quat`: 1 sqrt + ~10 fp ops (Shepperd's branches)
- `quat_slerp`: 1 acos + 2 sin + 4 mul (or 4 mul + 1 sqrt for nlerp fallback)
- `quat_to_mat3x3`: ~20 fp ops
- `compose_interp_matrix`: 2× of above (view + world) ≈ ~150 fp ops + 2 acos + 4 sin

Per-frame total ~250 fp ops + 2 acos + 4 sin = ~5 µs at modern clock
speeds. Negligible at 240 Hz (4 ms frame budget; we use 0.1% of it).

If profiling later shows this matters, replace acos/sin with their
Taylor-series small-angle approximations — slerp's hot path uses them
in a narrow [0, π/2] range where cubics are plenty accurate.

## Files touched

| File | Change |
|------|--------|
| `src/mtr-asi/include/mtr/interp.h` | adds `view_interp_enabled()`, `set_view_interp_enabled()`, `view_interp_writes()` |
| `src/mtr-asi/src/interp.cpp` | adds quaternion + slerp + `compose_interp_matrix` + write block in hk_camera_apply_all_active POST |
| `src/mtr-asi/src/sim_decouple.cpp` | exposes `effective_is_throttling()` as public forwarder over the anon-ns impl (other TUs need it) |
| `src/mtr-asi/src/menu.cpp` | "Enable view interp" checkbox + write counter; CAM:ON flag in FPS overlay |

## Validation

To confirm M3.1 is doing its job:

1. **Throttle ON**, target=60, FPS limit 240, **view interp OFF**:
   eyeball the camera. Should look 60 Hz fluid (steppy).
2. **View interp ON** (same other settings):
   camera should look 240 Hz fluid (smooth).
3. Check overlay flag `CAM:ON` lights up.
4. Check `Frames written:` increments at the render rate.

If camera glides through a hard cinematic cut, lower `Translation`
threshold (M2 sliders). If walking triggers cuts, raise it.

If aim feels laggy, that's the inherent lerp latency — M3.2 will fix
it once aim-mode detection is RE'd.

## See also

- [`decouple-m2-snapshot-infra-2026-05-07.md`](decouple-m2-snapshot-infra-2026-05-07.md) — M2 dependency
- [`high-fps-decoupling-plan.md`](high-fps-decoupling-plan.md) §M3 — original spec
- `memory/project_decouple_m3_shipped.md` — short pointer

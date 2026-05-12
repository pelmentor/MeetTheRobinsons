# dt-correctness — the root cause of "everything's janky at any rate"

Date: 2026-05-07
Status: investigation complete; implementation pending.

## TL;DR

The Wilbur engine integrates ~150 different per-tick subsystems against a
**hardcoded** `flt_6FFCBC = 0.003` dt. Physics, animation, particles,
pathcam, chain physics, screen shake, UV scroll, HUD tweens, every
critically-damped-spring follower — they all read this single global as
their fixed timestep.

The engine ships authored for an effective ~333 Hz pump rate
(1 / 0.003). At any other pump rate everything is mis-scaled in one
direction or the other. Worse: throttling a subsystem (e.g., pathcam at
sim Hz) at fewer calls per real-second further reduces its
"integrated spring time per real-second", making the system feel
sluggish.

**The fix is single-pointed**: write `flt_6FFCBC` to the per-consumer
`real_dt_since_last_call` just before each consumer runs. Then *all 150
subsystems* automatically become framerate-independent.

This is by far the highest-leverage change available in this codebase.

## How the problem was diagnosed

User reported: "camera is JANKY AT 60 HZ SIM, char is JANKY and gets blurred."

We had attributed this to interp / fence violations / view-matrix lerp
math. Wrong. The actual root cause:

1. Interp infrastructure was correct. `wrap_SetTransform(0x724C10)` (in
   `game_render_main_scene`) confirms the view matrix global IS the
   live D3D path. View interp writes are landing where they should.
2. The visible jank comes from PathCam's spring smoothing being
   frame-rate dependent + throttled.
3. PathCam's `sub_405630(target, current, ..., 0.003, react_rate)` is a
   pseudo-exponential smoother: `out = current + dt/(dt+rate) * (target - current)`.
   With dt=0.003 and rate=0.5, per-call blend = 0.6%. At 60 calls/sec the
   per-real-second response converges at ~30%/sec; at 240 calls/sec it's
   ~87%/sec. **A 4× responsiveness shift purely from call rate.**
4. We were *throttling* pathcam at sim Hz (60), so the spring was at the
   slow end of that range. That's the visible "camera lag".

When we then traced sub_405630's other 9 callers and walked the
flt_6FFCBC xref list, the realization landed: this is universal, not a
PathCam quirk.

## The single global: `flt_6FFCBC`

- VA: `0x006FFCBC`
- Value: `0.003` (= bytes `A6 9B 44 3B` little-endian, verified in
  binary)
- Only one bytes-pattern hit in the entire binary at the constant value
  itself; every disassembly that "uses 0.003" actually does `fld dword
  ptr [flt_6FFCBC]`. Patching the global at runtime is sufficient.
- Xref count: **153 readers**.

### Major consumer set

(All addresses are RVA + 0x400000 imagebase.)

| Consumer | VA | Notes |
|---|---|---|
| `physics_state_machine_tick` | `0x4DC150` | Multi-state physics; uses 0.003 for every velocity/acceleration scale and timer countdown. Sim path. |
| `entity_transform_tick` | `0x4B9F60` | Walks `dword_724DE4` transform list, writes entity+0x58 / +0x70 from anim+bone state. Sim path. |
| `anim_controller_advance` | `0x4E2B00` | `time += 0.003 * rate`. Per-track animation advance. Sim path. |
| `pathcam_smooth_pretick` | `0x58C000` | The camera spring. Render path (or throttled). |
| `chain_physics_tick_pass` | `0x4AE300` | Cape/cloth/banner solver. Alt-pump path. |
| `managed_object_list_tick` | `0x4B3BC0` | Per-element vtable[11](dt=0.003). Alt-pump path. |
| `wave_grid_tick` | `0x4B15C0` | 2D water/ripple grid. Alt-pump path. |
| `particle_buckets_sweep_a/b` | `0x4BAA40 / 0x4D3E50` | Particle FX. Sim path. |
| `trail_subsystem_tick` | `0x4D1D60` | Trails behind moving objects. Sim path. |
| `render_uv_scroll_tick` | `0x4C24E0` | Per-overlay-element UV/scroll integrator. Render path. |
| `tick_2d_overlay_pass` | `0x4A9F10` | HUD tween dt-integrator. Render path. |
| `render_screen_shake_tick` | `0x4D1FB0` | Computes real_dt then discards it for 0.003. Render path. |
| `timer_wheel_pretick` | `0x681380` | Engine timer wheel countdown. Alt-pump path. |
| `post_render_entity_sweep` | `0x596610` | Per-entity sweep with 0.003 dt. Main pump POST-render. |
| `alt_pump_subsystem_sweep` | `0x602F10` | Alt-pump subsystem walk. |
| `alt_pump_pre_sim_audio_sweep` | `0x6582F0` | Alt-pump pre-sim audio sweep. |
| `frame_dt_ring_update` | `0x584780` | Updates a 15-slot ring of `0.06666 sec * elapsed_ms`. Sim aggregator entry. |

### What `dword_6FFCA4` is

Separate from `flt_6FFCBC`: this is the **real elapsed milliseconds per
pump iteration**, written by `sub_57A2C0` at the start of every pump.

- Used by `simulation_tick_aggregator`'s tail call
  `sub_60EED0(dt = dword_6FFCA4 * 0.001)` — these subscribers (audio
  fades, music sync, shader uniforms) DO advance at real-time.
- Used by `particle_buckets_sweep_a` which captures BOTH
  `*(this+0)=flt_6FFCBC` and `*(this+4)=dword_6FFCA4*0.001` into the
  particle bucket context — engine devs preserved both because some
  particle paths use one and some use the other.

So the engine has **two parallel time sources** and 90% of subsystems
read the wrong one (the fixed-step).

## Why our existing throttle made things worse

`sim_decouple` currently throttles 11 hooks:
- sim_aggregator @ sim_hz
- pathcam_smooth_pretick @ sim_hz
- tick_2d_overlay_pass via sim-ran flag
- render_uv_scroll_tick via sim-ran flag
- wave_grid_tick / chain_physics_tick_pass / managed_object_list_tick
  via sim-ran flag
- timer_wheel_pretick / post_render_entity_sweep /
  alt_pump_subsystem_sweep / alt_pump_pre_sim_audio_sweep via sim-ran
  flag

**Throttling makes each consumer fire FEWER times per real-second**.
If consumer integrates with `dt = 0.003` per call, fewer calls means
less real-time advance. So at sim_hz=60 and pump_hz=240, throttled
consumers run at 60 calls/sec × 0.003 = 0.18 sec/sec — only 18% of
authored real-time.

This is the universal cause of "world feels slow / sluggish / janky"
under our throttle.

## The fix

Make `flt_6FFCBC` dynamic. Per consumer, before its orig runs, write
`flt_6FFCBC = real_elapsed_seconds_since_last_call_to_THIS_CONSUMER`,
clamped to `[MIN_DT, MAX_DT]` for safety.

After this fix:
- A subsystem fired every pump iteration sees `dt = pump_period`. It
  integrates correctly per real-second regardless of pump rate.
- A subsystem fired at throttled rate (e.g., sim_aggregator @ 60 Hz when
  pump is 240 Hz) sees `dt = 1/60 = 0.0167s`. It integrates 0.0167 ×
  60 = 1.0 sec/sec correctly.

Two complementary write sites are sufficient:

1. **Pump-wide write at `sub_57A2C0`**: After orig runs (`dword_6FFCA4`
   is now real elapsed ms), write `flt_6FFCBC = dword_6FFCA4 * 0.001`.
   This corrects every consumer that fires once per pump iteration
   (alt-pump consumers, render-path consumers, screen shake, etc.).

2. **Per-sim write at `simulation_tick_aggregator` PRE**: Override the
   value with `real_dt_since_last_actual_sim_run` (we already track this
   in M1.6 for `dword_6FFCA4`). This corrects sim-path consumers
   (entity_transform, physics, anim) when sim is throttled and runs
   less often than pump.

That's it. Two writes correct ~150 subsystems.

## Pathcam: special-case under dt-correctness

With dt-correct flt_6FFCBC, pathcam should NOT be throttled. Reasons:
- The spring becomes framerate-independent at any call rate.
- More calls/sec = smoother camera follow (no visible stepping).
- No reason to skip pathcam calls; they're cheap.

Action: remove pathcam from the throttle list. Fire it every pump
iteration. Spring response will match the authored feel at any FPS.

## Sim throttle: keep as opt-in performance feature

Why keep sim throttle at all if everything is dt-correct?
- Performance. Running physics + anim + particles at 240 Hz wastes CPU
  on a relatively old game.
- Determinism. Some game logic might have rate-dependent quirks we
  haven't found yet (e.g., AI script tickers, level-flag check timing).

Recommendation: keep sim throttle as a user-controlled option, default
to ON at 60 Hz (game runs sim at authored rate). With dt-correct, sim
runs at real-time speed regardless.

## Render-side smoothing: still wanted

Once the sim runs at correct real-time speed, the render layer can lerp
view+entity transforms at render rate (existing M3.1/M4/M5 work). The
combination is:
- Sim computes "real-time-correct" state at sim Hz.
- Render lerps between sim ticks for visual smoothness at render Hz.

This is the textbook "Fix Your Timestep" pattern, finally working
end-to-end.

## Sim throttle pattern: switch to accumulator

Current `should_skip` is hard-edge `if (now - prev < target_dt) return
true`. Near sim_hz ≈ render_hz, this gates inconsistently because
render frame timing isn't perfectly periodic.

Replace with accumulator:
```
accumulator += real_dt_since_last_check;
fire_count = floor(accumulator / step);
accumulator -= fire_count * step;
```

This guarantees regular sim cadence at any rate.

## Risks

- **Some subsystems may rely on the hardcoded 0.003** (numerical
  stability, hand-tuned magic numbers). Most won't, but we should clamp
  dt to a safe range: `MIN_DT = 0.0005` (skip if too small),
  `MAX_DT = 0.0667` (15 Hz floor, prevents physics tunneling /
  animation skipping).
- The user can toggle dt-correctness off if it breaks something.
- Animation rates were authored against 0.003 × call_rate; if the
  call_rate was 333 Hz that's "1.0 sec advance per real-second" =
  authored speed. dt-correct gives the same. ✓
- Spring rates similarly. ✓
- Physics velocities are in "world units per real second", so dt-correct
  preserves their meaning. ✓

## Open questions for next session

1. What's the engine's "natural" pump rate without our intervention?
   (sub_57A2C0 has a `time_ms - 17` floor suggesting ~60 Hz vsync
   fallback, but we run with vsync off via dxwrapper, so probably much
   higher.)
2. Are there subsystems where 0.003 is hardcoded to a magic number that
   shouldn't scale (e.g., a physical constant disguised as a dt)?
3. Does the engine have any per-tick velocity decay / damping that
   becomes UNSTABLE at large dt? (Implicit Euler is stable; explicit
   Euler isn't.)
4. The fence assumption (M4/M5): with dt-correctness restoring authored
   feel, the user may no longer perceive "char janky and blurred", in
   which case the fence verification work becomes a future polish item.

## Implementation plan

(Tracked in ../tasks/decouple/dt-correctness.md.)

Phase 1: dt-correctness module + sub_57A2C0 hook + sim_aggregator extension. Pathcam unthrottle. Build + deploy.
Phase 2: Accumulator-pattern sim throttle.
Phase 3: Validate runtime + iterate per-subsystem if any breaks.
Phase 4: Optional render-side smoothing pass.

## See also

- `decouple-0003-exhaustive-audit-2026-05-07.md` — the original 0.003
  audit that found 153 callers but didn't connect them to "the engine
  is uniformly framerate-broken".
- `decouple-architecture-self-assessment-2026-05-07.md` — the
  predecessor analysis that identified the issue but defaulted to
  "speculation" status.
- `high-fps-decoupling-plan.md` — original 7-milestone plan.

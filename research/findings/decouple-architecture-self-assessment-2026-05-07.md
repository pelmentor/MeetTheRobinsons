# Decouple architecture self-assessment

Honest read of the approach we built. Not a defense, not a marketing
doc — what's solid, what's questionable, what we'd do differently with
a clean slate.

## Approach summary

We built a **"Fix Your Timestep" pattern** (Glenn Fiedler 2004)
implemented at **call-site granularity** rather than pump granularity:

- 12 throttle hooks gating individual integrators on a sim-cadence flag
- Snapshot infra capturing prev/curr at sim-tick boundaries
- Lerp/slerp interp at render-frame granularity (M3 view, M4 player, M5 NPCs)
- Save-write-restore fence around state the sim reads back (M4/M5)
- 1 dt-correction patch (M1.6) for `dword_6FFCA4`
- Plus several complementary toggles: aim-snap, mini-game auto-disable,
  cut/teleport detection, preset profiles

This IS the textbook architecture used by Bloodborne 60-fps, GameCube/
PS2 emulator high-FPS hacks, and basically every "fix the engine to
render at higher rate" mod. It's not novel — it's a known-correct
pattern.

## What's solid

1. **Pattern is correct.** Fixed-timestep + interpolation is THE answer
   for engines authored at one rate that need to render at another.
2. **Targeted hooks vs pump rewrite is a good trade-off for a mod.** We
   gate engine code rather than replacing it. Less risk of breaking
   compat, no risk of touching SecuROM-thunked or anti-tamper paths.
3. **Default OFF, granular toggles, escape hatch (Compatibility preset).**
   The user can disable any individual layer or all of it, so a
   misbehaving hook can't lock them out of the game.
4. **Auto-disables on mini-games and cut detection.** Two layers of
   "if we don't know how to handle it, get out of the way" safety.
5. **Static coverage of 0.003 sites is exhaustive** (per the audit doc).
   No SecuROM excuses; we know we got everything visible.

## What's questionable

### 1. `target_hz` slider is misleading (real UX bug)

The slider's name implies fidelity ("how smooth is sim?"). But the
underlying engine has a **fixed 0.003 step** — physics integrates
`pos += vel * 0.003` per sim call, regardless of how often we call
sim. So:

| User sets target_hz | Effect |
|---------------------|--------|
| 60 | Calls match authored cadence → game speed correct ✓ |
| 30 | Calls half as often → physics integrates half as fast → **slow-mo** |
| 120 | Calls 2× → physics 2× faster → **fast-forward** |

A user picking 120 expecting "smoother physics" gets a 2× speed game.
Only 60 is correct.

**This is a real bug.** Two fixes:
- **Cheap**: lock the slider to 60, rename it "Reset throttle (read-only)"
  or remove it entirely. One-line change.
- **Right**: implement an actual accumulator (`while (acc >= 1/60) {
  run_sim(); acc -= 1/60; }`) so call rate adjusts to give correct speed
  at any user-chosen rate. ~50 LOC change.

We picked neither yet. The slider currently lies about what it does.

### 2. The fence pattern is unverified

`pre_sim_restore_player()` and `pre_sim_restore_npcs()` assume the
engine doesn't read entity pos/rot between our camera-apply-POST write
and the next sim's PRE-restore. I haven't proven this. If any render-
path code reads entity transforms (animation update? scene-graph
visibility?), our writes would feed it interp values where it expected
fresh sim values.

Likely fine — most render paths read globals (which is what M3
interpolates for cameras) rather than entity state directly. But
"likely fine" is not "verified".

### 3. M1.7 / M1.8 throttles are speculative

I assumed skipping these on non-sim frames is safe:
- `timer_wheel_pretick` (scheduled callbacks)
- `post_render_entity_sweep` (post-render bookkeeping)
- `chain_physics_pass` (cape/cloth)
- `wave_grid_tick` (water)
- `managed_object_list_tick` (timed effects)
- `alt_pump_subsystem_sweep`, `alt_pump_pre_sim_audio_sweep`

For each, the assumption "skipping is benign" was a guess. Skipping
`timer_wheel_pretick` means scheduled callbacks fire later than
authored — could cause visual glitches in cinematic state transitions
that depend on millisecond-accurate timing.

No static evidence either way. Runtime testing required.

### 4. 12 hooks is a lot of debug surface

When something misbehaves under throttle, we have 12 places to look.
A single accumulator-in-pump approach would have ONE place. Granted,
the hooks are simple (each is `if (skip) return; else call_orig`), but
it's still more surface than ideal.

### 5. None of this has been runtime-tested yet

All confidence above is theoretical. Real bugs only show up when the
code runs against actual gameplay. Expectation calibration: at least
some of the 12 hooks will have bugs we discover during validation.

## What we'd do differently with a clean slate

**Single accumulator-in-pump hook.** Hook `sub_572040` (the main pump)
ONCE; inject:

```cpp
hk_pump_iteration:
    real_dt = qpc_now() - last;
    last = qpc_now();
    accumulator += real_dt;

    while (accumulator >= 1.0 / 60.0) {
        sub_57A2C0();          // dt prep
        sub_67F430();          // sim aggregator (which calls all the
                               //  alt-pump-style integrators inside)
        sub_609B90();
        accumulator -= 1.0 / 60.0;
    }
    // sub_682010's pre-sim integrators handled by hooking that pump too

    render_alpha = accumulator * 60;
    render_frame_top_level();  // with interp(prev, curr, alpha) writes
```

**Pros:**
- ~150 LOC vs our 1600 LOC
- Single hook, single debug surface
- target_hz becomes a real fidelity knob (calls 0+ times per render to
  match real time → speed always correct)
- Implicit handling of every integrator inside `sim_aggregator` — no
  need to enumerate 12 of them separately

**Cons (why we didn't):**
- Touches the pump's control flow directly (more invasive)
- Render alpha must be threaded into M3/M4/M5 interp (same plumbing
  needed though)
- Alt pump has a parallel structure — would still need a second hook
  for `sub_682010`

The accumulator approach absorbs M1.4+M1.7+M1.8 into "you don't run
sim more often than 60Hz, period" which is mathematically equivalent
to all our individual throttles combined.

## When does our approach beat the accumulator approach?

When **the user wants to throttle sim BELOW the authored rate** to
slow the game (e.g. "30Hz target for a slow-motion bullet-time
effect"). Our approach delivers slow-mo trivially because target_hz
controls call rate, not real-time-rate. The accumulator approach
always plays at real-time rate.

But that's a niche usecase, probably not what the user wants. The
common case (high refresh rate without speed change) is what the
accumulator does correctly and our approach gets only at target_hz=60.

## Pragmatic next steps

1. **Test what we have.** Most likely it works at target_hz=60 in the
   common case. That covers ~95% of usecases.
2. **If it works**, ship as-is and add the slider docs (or lock it to
   60) — the complexity is paid for, no need to refactor.
3. **If hooks misbehave**, refactor to the accumulator pattern. We
   have all the interp infra reusable from M2/M3/M4/M5; only the 12
   throttle hooks would be replaced by one pump hook.

## Calibration on confidence

| Aspect | Confidence |
|--------|------------|
| 0.003 site coverage is exhaustive | High (verified via 7 search vectors) |
| target_hz=60 + render=240 works correctly | High (matches authored cadence exactly) |
| target_hz≠60 works as user expects | **Low** (it's a speed dial, not a fidelity dial) |
| M3/M4/M5 interp math is correct | Medium-high (standard slerp/lerp + verified offsets) |
| Save-write-restore fence is race-free | Medium (logical analysis only, not verified) |
| M1.7/M1.8 throttles don't cause secondary bugs | **Medium-low** (speculative skips) |
| First playtest will be flawless | **Low** (12 hooks × untested behaviour = bugs) |

## Recommendation to user

**Test the Quality preset first.** That sets target_hz=60 and enables
all interp — the configuration we're most confident in. If it works,
we have the headline feature. If it doesn't, the failure mode tells us
which layer to fix.

**Don't change target_hz from 60 yet.** Until we either lock it or
implement the accumulator, picking other values is misleading.

## See also

- [`high-fps-decoupling-plan.md`](high-fps-decoupling-plan.md) — original engineering plan
- [`decouple-0003-exhaustive-audit-2026-05-07.md`](decouple-0003-exhaustive-audit-2026-05-07.md) — coverage proof
- `memory/project_decouple_plan_complete.md` — milestone status
- `memory/project_decouple_architecture_self_assessment.md` — pointer to this doc

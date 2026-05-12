# dt-correctness — runtime test plan

Use this when validating the dt-correctness build (Game/mtr-asi.asi >=
546304 bytes, deployed 2026-05-07 Phase 3). Each test is a concrete
check the user can run and observe; the expected outcome distinguishes
"fix worked" from "fix broke something" from "another problem still
present".

## Phases shipped

| Phase | What | Build size |
|---|---|---|
| 1 | dt_correctness module + accumulator + throttle bypass | 540160 |
| 1.5 | Player handle staleness + fence-violation diagnostic | 541184 |
| 1.6 | Camera-world-space view interp | 542720 |
| 2 | HUD staleness fix (dword_6FFCA4) + sub_4F45F0 particle hook + TimeScale | 545792 |
| 3 | Alt-pump dt write via timer_wheel_pretick + diagnostics | 546304 |

## Phase 2/3 specific checks (NEW)

### Check 9 — HUD staleness fix

Originally: at sim_hz=15, animated UI elements stepped at 15 Hz.
After Phase 2: UI should be smooth at 240 fps.

1. Set Recommended preset (dtc on, target=60, render=240).
2. Open menu, find an animated UI element (HUD timer, fade, scrolling
   text, anything that moves continuously).
3. Watch its motion at 240 fps render.

Now reduce target_hz to 15 Hz with dtc still ON:
- Expected: UI animations REMAIN smooth at 240 fps.
- If still steppy at 15 Hz: dword_6FFCA4 isn't being updated from
  render path. Check menu's "Render-path writes" diagnostic — should
  be growing. Check "last %.4f s" — should be ~0.0042.

### Check 10 — Particle integrator hook

1. Set sim_hz=15, dtc on, time_scale = RealTime.
2. Watch particle effect (smoke, sparks, anywhere).
   - Expected: particles run at 1.0× real-time = smooth visually.

3. Switch time_scale to SlowMoAtLowSim.
   - Expected: particles slow to 0.25× speed (sim_hz=15 / 60 = 0.25).
     Visually still smooth at 240 fps (engine renders particles every
     frame), but motion is 4x slower than normal.

4. Switch time_scale to Off.
   - Expected: particles fast (vanilla 0.003 fixed step × 240 calls = 0.72/sec).

If particles don't change speed across modes, sub_4F45F0 hook isn't
firing. Check menu's "Particle dt scaled" counter — should grow when
time_scale != RealTime.

### Check 11 — Alt-pump consumers

Affected systems: 2D wave grid (water), chain physics (cape/cloth on
NPCs), screen shake, timer wheel.

1. Find a level with water (any outdoor scene with a pond/pool).
2. Find a character with cape/cloth (NPC with flowing clothing).
3. Set sim_hz=15, dtc on.

Time scale = RealTime: water ripples and cloth swing at real-time.
Time scale = SlowMoAtLowSim: water + cloth slow to 0.25× speed.
Time scale = Off: water + cloth at engine default rate.

Diagnostic: menu's "Alt-pump-path writes" counter should grow at
~240 Hz (= alt-pump rate). "last" should be ~0.0042 sec.

If alt-pump writes is 0, the hook on timer_wheel_pretick isn't firing
or alt-pump isn't running.

### Check 12 — Combined behavior

The simplest sanity check: with dtc ON + RealTime mode + sim_hz set
to anything (15, 30, 60, 120, 240):

- World plays at the SAME real-time speed (1.0×).
- Visual smoothness is DETERMINED by render rate (= FPS limiter).
- sim_hz controls FIDELITY (higher = finer collision/AI/physics resolution).
- HUD, camera, particles, water, cloth — all play at real-time.

If anything visibly slows or speeds based on sim_hz alone, dt-correctness
isn't reaching that subsystem.

## Setup

1. Launch the game. Press `Insert` to open the menu.
2. Click `Recommended` preset. This sets:
   - Engine timestep correction: ON
   - Lock game logic to 60 Hz: ON, target = 60 Hz
   - Smooth camera between sim ticks: ON
   - Smooth Wilbur / Smooth NPCs: OFF
3. Open `Tools` tab, set FPS limiter to **240** (or whatever your
   monitor refresh allows) — high render rate is what makes the
   smoothing visible.
4. Close menu, drop into a level (Garage, Robinson House, anything
   with Wilbur walking around).

## Check 1 — pump rate sanity

Open menu, in `High-FPS smoothing` section. Look for the new live
readings under "Engine timestep correction":

```
Render-path writes: <N> (last 0.0042 s)
Sim-path writes:    <M> (last 0.0167 s)
```

Expected:
- Render-path "last" ≈ `1 / your_fps` (e.g. 0.0042s = 240 Hz, 0.0083s
  = 120 Hz, 0.0167s = 60 Hz).
- Sim-path "last" ≈ `1 / target_hz` (= 0.0167s when target = 60).
- Both `<N>` and `<M>` should be growing rapidly while the menu is
  open (one per render frame / one per sim tick).

If render-path writes don't grow → the camera_apply hook isn't firing
(likely interp module install failed; check `Game/mtr-asi.log` for
"interp: hooked camera_apply_all_active").

If sim-path writes don't grow → sim_aggregator hook isn't firing
(check log for "sim_decouple: hooked simulation_tick_aggregator").

## Check 2 — camera responsiveness vs vanilla

Goal: the original "camera janky at 60 Hz sim" should be GONE.

1. Walk Wilbur in a straight line, moderate pace.
2. Make abrupt 90° turns left/right while walking.
3. Watch how quickly the camera swings to follow.

Expected with dt-correctness ON:
- Camera follow feels natural and snappy.
- No visible lag or "rubber-banding" between Wilbur's motion and
  camera-recenter.
- At 240 Hz render, motion should look perfectly fluid (no stepping).

Compare vs OFF:
- Click `Off` preset. Walk and turn the same way.
- Camera should now feel either too sluggish (60 Hz logic, no smoothing)
  or fast (no logic cap = pump rate). This is the vanilla / pre-fix
  baseline.

If WITH dt-correctness ON the camera feels worse than OFF → something
is wrong; likely the spring is being fed an unexpected dt. Open menu,
read render-path "last" — confirm it matches your FPS.

## Check 3 — non-60 sim_hz speed correctness

Goal: dt-correctness should make any sim_hz feel real-time.

1. Set `Game logic rate` to 30 Hz.
2. Walk Wilbur. Time how long it takes to traverse a recognisable
   distance (e.g. across a room, ~10 seconds at 60 Hz).

Expected with dt-correctness ON:
- 30 Hz: same traversal time as 60 Hz (both ~10 sec). World runs at
  real-time. Animation may look slightly choppier (fewer logic ticks)
  but speed is correct.
- 120 Hz: same traversal time. Animation looks smoother.
- 240 Hz: same traversal time. Highest fidelity.

Expected with dt-correctness OFF:
- 30 Hz: half-speed (slow-motion) traversal — ~20 sec.
- 120 Hz: 2× fast-fwd traversal — ~5 sec.
- 60 Hz: correct speed.

If dt-correct traversal at 30 Hz is faster or slower than 60 Hz, the
sim path's flt_6FFCBC write isn't reaching all sim consumers.

## Check 4 — Wilbur smoothing + fence diagnostic

Goal: see if M4 fence assumption is OK on this engine.

1. Open menu, enable `Smooth Wilbur (player) between sim ticks`.
2. Walk Wilbur for ~10 seconds.
3. Open menu, look at the fence diagnostic line:
   ```
   Frames written: <N>  teleports: <T>  handle swaps: <S>
   Fence violations: <V>
   ```

Interpretation:
- V == 0: fence is clean, M4 is safe on this engine. Wilbur smoothing
  should look correct (no jank/blur). User can leave it on.
- V > 0 but small (<10): rare violations, possibly transient. Watch
  whether Wilbur looks janky during those frames.
- V grows continuously (V keeps climbing every frame): another engine
  subsystem modifies Wilbur's transform between our write and next
  sim. M4 IS UNSAFE on this engine — turn off `Smooth Wilbur`. Future
  work needs a "shadow render" pattern instead.

Also note `handle swaps`: should be 0 in single-level gameplay. If
it's non-zero without a level/mode change, the player handle is being
invalidated unexpectedly (could indicate entity churn or our refresh
interval is too aggressive).

## Check 5 — mode switch (Wilbur ↔ MiniHamsterPlayer ↔ DigDug)

Goal: verify the player handle handles entity replacement.

1. Drop into a mini-game (DigDug or MiniHamster).
2. Open menu, look at `Snapshots / Cuts / handle swaps` lines.
3. Note `handle swaps` value (should auto-disable interp under
   mini-game; minigame_detected check vetoes throttle).
4. Exit mini-game back to actor mode.
5. Re-open menu. `handle swaps` should have incremented at least
   once during the mode transition.

If `handle swaps` is 0 across a mode transition, our handle staleness
detection isn't catching the entity swap → snapshots could carry
stale data into the new mode. Click `Force player relookup` button to
manually refresh. If smoothing then looks correct, the auto-detection
threshold needs tightening.

## Check 6 — engine reboot under accumulator-pattern throttle

Goal: verify accumulator replaces the previous hard-edge "fires 0 or
2 times per window" jitter.

1. Set `Game logic rate` = 60. Set FPS limiter = 60. (Worst case:
   sim_hz ≈ render_hz.)
2. Open menu, look at `BLEND` value in the FPS overlay corner.
3. Watch BLEND for 10 seconds.

Expected:
- BLEND should stay close to a stable value (ideally 0.0 since alpha
  re-zeros each sim tick at sim=render rate). May briefly tick up to
  ~0.3 then reset.
- BLEND should NOT bounce wildly between 0 and 1 (that's the old hard-
  edge jitter).

If BLEND is jittery: accumulator pattern didn't take effect (verify
build deployed). Check log for "accumulator-pattern".

## Check 7 — camera-world-space view interp A/B test

Goal: confirm the math-correct view interp doesn't regress vs the
old direct-matrix-lerp path.

1. Recommended preset on (view interp ON, dtc ON, target = 60 Hz, FPS
   = 240).
2. Walk Wilbur with the camera doing fast turns.
3. Open menu, toggle `Use camera-world-space lerp (math-correct)` ON
   vs OFF, watching how the camera looks.

Expected:
- ON (default): mathematically correct camera path. Smooth.
- OFF: same scene; visually nearly identical at small windows. Mostly
  a sanity check that both paths work.

If ON looks WORSE than OFF (e.g., camera drifts, jumps, or the
horizon tilts strangely), the camera-world-space math has a sign bug
or row/column-major mismatch. Report which axis or motion triggers
the artifact.

## Check 8 — dt-correctness off → vanilla regression

Goal: ensure our changes don't break the vanilla path.

1. Click `Off` preset.
2. Confirm `Engine timestep correction` checkbox is OFF.
3. Confirm sim_hz = 60 isn't enforced (slider stays at last value).
4. Walk Wilbur. Speed should match vanilla 0.003 fixed-step behaviour
   (same as without our mod loaded at all).

If Off-preset behaviour differs from "no mod loaded": one of our
hooks is leaking work even when dt-correctness is off. Re-examine
[interp.cpp](../src/mtr-asi/src/interp.cpp) and
[sim_decouple.cpp](../src/mtr-asi/src/sim_decouple.cpp) for unconditional writes.

## What to report back

After running the checks, the most actionable feedback is:

```
Recommended preset (dtc on, target=60, view interp on):
- Camera responsiveness vs vanilla: [better / same / worse]
- "Char janky / blurred" original report: [GONE / improved / same]

All smoothing preset (player + npc interp on):
- Wilbur smoothness: [smooth / occasional jank / persistent jank]
- Fence violations counter after 30 sec: [0 / small / grows]

Off preset:
- Behaves vanilla? [yes / no]

Pump-rate diagnostics at 240 fps render, 60 hz logic:
- Render-path last:   [should be ~0.0042]
- Sim-path last:      [should be ~0.0167]
- Render-path writes growing? [yes / no]
- Sim-path writes growing? [yes / no]
```

That's enough for me to know if Phase 1 worked, partial-worked, or
fell over.

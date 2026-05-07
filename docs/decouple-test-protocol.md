# Decouple validation protocol (M0.3)

Manual test protocol for validating the high-FPS render decouple. Six tests
cover gameplay correctness (jump height / walk speed / anim timing), camera
smoothness, aim responsiveness and cutscene behaviour. Each milestone in the
project plan acceptance gate refers back here.

This protocol is written so a non-coder user can run it: it tells you which
toggles to set, what to do in-game, and what to watch for.

## Prerequisites

1. Latest `Game/mtr-asi.asi` deployed.
2. FPS limiter functional (Tools tab → "FPS limiter" section).
3. Decouple controls visible (Tools tab → "Decouple sim from render").
4. Performance overlay visible (Tools tab → "Performance overlay" → on).
   When the decouple is in THROTTLE mode, the overlay expands to show
   RENDER / SIM / ALPHA lines + status flags.
5. Reference saves present (see [decouple-saves.md](decouple-saves.md)).
6. (Optional but recommended) screen-recording capability for side-by-side
   comparison of 60 / 120 / 240 Hz captures.

## Test matrix

Run every test at four render rates: **30, 60, 120, 240 Hz**. The throttle
target Hz stays at **60** in every case (it's the engine's authored cadence).

### Setup before each rate

| Setting                       | Value                            |
|-------------------------------|----------------------------------|
| Tools → FPS limiter           | `Limit FPS` ON, target = the test rate |
| Tools → Decouple → Throttle   | ON when testing M1+, OFF for baseline regression |
| Tools → Decouple → Target Hz  | 60 |
| Tools → Performance overlay   | ON (visible while playing) |

Always run a **baseline pass** first with throttle OFF at 60 Hz to confirm
the build matches the user's prior expectations. Then enable throttle and
run the rates.

## Tests

### T1 — Jump height

**Procedure**:
1. Load `decouple-T1-flat-ground` save (open level, flat terrain, no enemies).
2. Stand still.
3. Jump straight up (no horizontal movement).
4. Observe the peak height visually relative to a reference (a tree, doorway,
   wall texture seam — whatever's nearby and stable).

**Pass criteria**: peak height matches across all four render rates.

**Failure mode**: at higher rates Wilbur jumps higher (sim runs faster) →
either throttle is OFF, or there's a hidden 0.003 site we haven't covered
(M1.4 sweep targets these).

### T2 — Walk speed

**Procedure**:
1. Same save as T1.
2. Walk a fixed straight line from one known landmark to another (e.g. tree
   to fence post). Use a stopwatch or count frames in a recording.
3. Repeat at each render rate.

**Pass criteria**: traversal time matches across rates within ±5%.

**Failure mode**: walks are 4× faster at 240 vs 60 Hz → physics state machine
isn't being throttled.

### T3 — Animation timing

**Procedure**:
1. Pick a deterministic animation: jump-and-land has clean start/end frames;
   the slide attack also works.
2. Trigger it from a stationary position.
3. Time start-of-anim to end-of-anim with stopwatch (or count recording
   frames at the known capture rate).

**Pass criteria**: duration matches across rates.

**Failure mode**: animations play 4× faster at 240 Hz → `anim_controller_advance`
or `anim_evaluate_track` isn't being throttled.

### T4 — Camera follow / smoothness

**Procedure**:
1. Load `decouple-T4-circle-run` save (open area, room to run circles).
2. Run in a tight circle (constant turn rate).
3. Observe camera lag distance behind Wilbur.
4. Observe smoothness of camera motion (no stutter, no over-snap).

**Pass criteria**:
- Camera lag distance same at all rates (camera-spring constants honored).
- At 240 Hz with M3 view interp on, camera motion feels visibly smoother
  than at 60 Hz (this is the headline benefit).
- At 240 Hz with M1 throttle only (M3 view interp off), camera moves at
  60 Hz cadence — visually steppy, but lag distance and overall behaviour
  are correct.

**Failure mode**:
- Camera snaps to Wilbur instantly at 240 Hz → `pathcam_smooth_pretick`
  isn't being throttled (M1.3).
- Camera glides past Wilbur after a sharp turn → cut detection is too
  permissive, or M3 view interp is over-blending.

### T5 — Aim responsiveness

**Procedure**:
1. Load `decouple-T5-target-practice` save (a level with a stationary target
   in line of sight).
2. Aim a weapon at the target.
3. Snap-aim to the target (camera should lock on).
4. Compare felt latency to baseline 60 Hz.

**Pass criteria**: aim feels native at 240 Hz — no perceptible "drag" or
"glide" past the lock-on point.

**Failure mode**: aim glides past the target → view interp is interfering
with aim mode. M3.2 aim-mode snap (alpha=1 on aim) should fix this.

### T6 — Cutscene fidelity

**Procedure**:
1. Load `decouple-T6-cutscene-cut` save (game state just before a known
   cinematic with hard cuts).
2. Watch the cutscene at 240 Hz with all interp on.
3. Watch the same cutscene at 60 Hz baseline (throttle off).
4. Compare side-by-side.

**Pass criteria**: 240 Hz cutscene visually identical to 60 Hz baseline at
each cut — no "slide" through the cut, no double-image at high-delta frames.

**Failure mode**:
- Glide through a cut → cut detection threshold (M2.2) is too lenient.
  Tunable via UI (M6.2 explicit cutscene flag).
- Pop / stutter at every camera move (not just cuts) → threshold too
  strict, false-positive on normal motion.

## How to record results

Each test pass should produce a one-line entry in a tracking spreadsheet
or text file:

```
2026-05-XX  T1  baseline  60Hz   peak ~3.0m   PASS
2026-05-XX  T1  thr+M1    60Hz   peak ~3.0m   PASS
2026-05-XX  T1  thr+M1   120Hz   peak ~3.0m   PASS
2026-05-XX  T1  thr+M1   240Hz   peak ~3.0m   PASS
```

Side-by-side video clips (one per rate) make T4 / T5 / T6 visual comparisons
much faster than written notes.

## What to do when a test fails

1. **Confirm the build version**. Is `Game/mtr-asi.asi` the latest? Restart
   game after replacing it.
2. **Confirm the toggle state**. Does the overlay show DECOUPLE: THR:ON?
3. **Check the detailed log**. Tools → Decouple → "Detailed log" ON, repeat
   the failing test, then stop and read `Game/mtr-asi-decouple.log`.
4. **Open an investigation**. If T1/T2/T3 fails at 240 Hz, the symptom is
   "sim runs faster than throttle target": triage as a hidden 0.003 site
   for the M1.4 sweep. If T4 fails, triage as a `pathcam_smooth_pretick`
   throttling issue (M1.3). If T5 fails, M3.2 aim-mode handling. If T6
   fails, M2.2 cut detection or M6.2 cutscene flag.

## Why this protocol

The acceptance gates in [`research/findings/high-fps-decoupling-plan.md`](../research/findings/high-fps-decoupling-plan.md)
all reference these tests by name (T1, T2, ...). Keep this doc in sync with
that plan: when a milestone changes its acceptance criteria, update the
relevant test here.

The tests are deliberately low-tech (visual comparison, stopwatch). The aim
is for the user to run them in 30 minutes and confirm the build works as
claimed — not a research-grade benchmark.

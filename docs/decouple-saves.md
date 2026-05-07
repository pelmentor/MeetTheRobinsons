# Decouple validation saves (M0.2)

Reference saves used by the [decouple-test-protocol](decouple-test-protocol.md).
Each save loads to a known repeatable state used by one or more tests.

This document is a placeholder until the user records these saves in-game.
Once recorded, copy the save files into `research/test-saves/` and append
the file name + creation date to each row below.

## Save points

| ID | Used by | What loads | Save file |
|----|---------|------------|-----------|
| `decouple-T1-flat-ground` | T1 (jump), T2 (walk), T3 (anim) | Open level, flat ground, no enemies, no triggers nearby. Wilbur stationary, plenty of empty space. | _(not yet recorded)_ |
| `decouple-T4-circle-run` | T4 (camera follow) | Open area with at least 5 m radius of clear ground. PathCam unobstructed. Daytime / clear weather to keep camera shading consistent. | _(not yet recorded)_ |
| `decouple-T5-target-practice` | T5 (aim responsiveness) | A level segment with a stationary target in line of sight (a fixed object Wilbur can target). Weapon equipped. | _(not yet recorded)_ |
| `decouple-T6-cutscene-cut` | T6 (cutscene fidelity) | Just before a triggered cutscene with at least one hard camera cut. Cutscene plays deterministically each load. | _(not yet recorded)_ |

## Procedure for recording each save

1. In-game, navigate to the described state.
2. Use whatever native save mechanism the game offers (or a memory-snapshot
   tool if no save slot fits — TBD by user; see Cancellation note below).
3. Verify the save reproduces the state by reloading it three times in a
   row from a clean game launch.
4. Copy the save file to `research/test-saves/<ID>/`.
5. Update this doc with file name + recording date.

## Cancellation note

Wilbur (Meet the Robinsons 2007 PC) ships native save slots for the main
campaign. They should be sufficient for T1–T3 and T6. T4/T5 may need
specific positioning that the in-game save doesn't restore precisely
(camera angle, weapon state, etc.) — if so, document the manual setup
steps in the relevant test row instead of a save file. The tests are
deliberately lenient on absolute positioning — what matters is that the
*comparison* across render rates uses the same starting state, not that
that state is bit-identical to a previous run.

## See also

- [decouple-test-protocol.md](decouple-test-protocol.md) — the tests these
  saves serve.
- [`research/findings/high-fps-decoupling-plan.md`](../research/findings/high-fps-decoupling-plan.md) §M0.2 — the project plan section
  defining this work.

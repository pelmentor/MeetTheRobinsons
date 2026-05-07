# M1.7 — alternative-pump 0.003 sites

The earlier "static sweep complete" claim missed an entire **second
engine pump** at `engine_pump_alt` (sub_682010) which contains its own
trio of 0.003 integrators called BEFORE `simulation_tick_aggregator`.
This patch covers them.

## How the alt pump was missed

The original sweep walked `render_frame_top_level` callees and
xref'd `flt_6FFCBC`. It correctly identified the in-aggregator and
in-render sites. But `simulation_tick_aggregator` (0x67F430) has TWO
callers:

1. **`sub_572040`** (the main pump) — calls `sub_57A2C0` →
   `sim_aggregator` → `render_frame_top_level`. Has no per-frame 0.003
   integrators outside the aggregator.
2. **`engine_pump_alt`** (sub_682010, renamed today) — calls
   `engine_pump_alt_pre_sim` (sub_6813C0) → THREE 0.003 integrators →
   `sim_aggregator` → `render_frame_top_level`. The pre-sim integrators
   were invisible to the original sweep because xref-ing `flt_6FFCBC`
   doesn't catch literal-0.003 immediates passed as function args, and
   sub_682010 isn't reached by walking from `render_frame_top_level`.

The alt pump is dispatched via vtable (xref at 0x6791F0 is data → vtable
slot). State-machine-driven: probably a different game state than the
main pump. Without runtime trace, we don't know exactly when each pump
is active, but **throttling all three 0.003 integrators it owns is safe
in either case** (no-op when alt pump isn't active).

## What the alt pump runs before sim_aggregator

`engine_pump_alt` (sub_682010):
1. `sub_681380` — TBD
2. `sub_5AD4D0` — TBD
3. `engine_pump_alt_pre_sim(this)` (sub_6813C0):
   - `chain_physics_tick_pass` (sub_4AE300): walks `unk_71D33C` chain
     list, calls `chain_physics_solver(0.003, damping)` per chain.
     Cape / banner / cloth physics integrator with hardcoded 0.003 dt.
     Damping uses sub_57A060 (frame-rate aware), but the integration
     step itself doesn't.
   - `managed_object_list_tick` (sub_4B3BC0): walks linked list
     `*this`, calls per-object `vtable[8]` (status check, dt=0) +
     `vtable[11](dt=0.003)` (advance). Generic managed-resource ticker.
4. If `dword_71D790`: `wave_grid_tick(0.003)` (sub_4B15C0):
   2D Laplacian wave/ripple solver (water surfaces, force-fields). The
   inner solver `wave_grid_solver` (sub_4B0270) integrates pos += vel × 0.003.
5. `sim_aggregator`
6. `render_frame_top_level`

## The fix

Three new MinHook detours, each gating on
`!g_sim_ran_last_iteration` exactly like the M1.4 render-path
throttles. Cadence: previous sim_aggregator's run/skip decision
determines whether the alt-pump pre-sim integrators run this iteration.

```cpp
hk_wave_grid_tick      → skip when prev sim was decimated → 4× -> 1× wave
hk_chain_physics_pass  → skip when prev sim was decimated → 4× -> 1× cape
hk_managed_obj_list    → skip when prev sim was decimated → 4× -> 1× managed timers
```

When throttle is OFF: passthrough, no behavioral change.
When throttle is ON, alt pump active, render at 240 / sim 60: each fires
~60×/sec instead of 240×/sec — physics-correct.
When throttle is ON, alt pump NOT active: hooks fire 0×/sec (no-op).

## Renames in IDB

| VA | Old | New |
|----|-----|-----|
| 0x682010 | sub_682010 | `engine_pump_alt` |
| 0x6813C0 | sub_6813C0 | `engine_pump_alt_pre_sim` |
| 0x4B15C0 | sub_4B15C0 | `wave_grid_tick` |
| 0x4B0270 | sub_4B0270 | `wave_grid_solver` |
| 0x4AE300 | sub_4AE300 | `chain_physics_tick_pass` |
| 0x4ADBD0 | sub_4ADBD0 | `chain_physics_solver` |
| 0x4B3BC0 | sub_4B3BC0 | `managed_object_list_tick` |

## Symptoms this fixes (when alt pump is active at high render rate)

- **Water/ripple speed**: pond ripples, splash trails, force-field waves
  visibly tick 4× too fast at 240 Hz with throttle@60.
- **Cape/banner motion**: any flowing-cloth NPC (probably present in
  cinematic scenes) animates 4× too fast.
- **Managed-resource timers**: anything ticked through the
  `managed_object_list_tick` (decals, scripted timed effects) runs 4×
  too fast.

## Telemetry

Tools → Decouple → "Alt-pump: wave=N chain=N managed=N (M1.7)" — counts
how many times each integrator was decimated by our throttle. When
throttle@60, render at 240, alt pump active: expect ~180 skips/sec per
integrator (3 of every 4 calls skipped).

When all are zero, either throttle is off OR the alt pump isn't active
in your gameplay state.

## Why these weren't found by the static sweep

`flt_6FFCBC` xref hits sites that read the global. Literal `0.003`
floats (from compiler folding) appear as immediates without a memory
xref. The bytes for the 0.003 immediate as a `push 3C8B4396h`
instruction or as a memory operand to `fld dword [const]` may not show
up in the xref table.

To catch them: dis-assembly walking from a known live-code root. We
walked from `render_frame_top_level` initially. Walking from
`simulation_tick_aggregator` upward through its CALLERS surfaced the
alt pump.

Future audit pattern for hidden time-coupled work: **walk every caller
of the engine's primary tick functions, both up and down.**

## Files touched

| File | Change |
|------|--------|
| `src/mtr-asi/include/mtr/sim_decouple.h` | adds wave/chain/managed skip-counter accessors |
| `src/mtr-asi/src/sim_decouple.cpp` | three MinHook detours, gating on sim_ran flag |
| `src/mtr-asi/src/menu.cpp` | new "Alt-pump: ..." telemetry line |

## See also

- [`decouple-hidden-0003-sites-2026-05-07.md`](decouple-hidden-0003-sites-2026-05-07.md) — original M1.4 render-path sweep
- [`decouple-residual-systems-2026-05-07.md`](decouple-residual-systems-2026-05-07.md) — what was previously claimed "covered" (now updated)
- `memory/project_decouple_m1_7_alt_pump.md` — pointer

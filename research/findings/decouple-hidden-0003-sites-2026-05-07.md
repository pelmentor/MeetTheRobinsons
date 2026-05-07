# Hidden 0.003 sites in render path — M1.4 static sweep

Static sweep findings for the M1.4 acceptance step of the high-FPS
decouple plan. Goal: identify every site in the render path that integrates
with hardcoded `0.003` (or reads `flt_6FFCBC`) outside the
`simulation_tick_aggregator` call chain we already throttle, so they can be
gated at the same cadence as the aggregator.

Method: walked `render_frame_top_level @ 0x4D22D0` callees, cross-referenced
xrefs to `flt_6FFCBC` (0x6FFCBC), and inspected each candidate to confirm
the constant is used as a per-frame integration step (not as an epsilon
or threshold).

## Engine pump order (relevant invariant)

`sub_572040` (the engine pump):

```
while (running) {
    sub_57A2C0()                  // per-frame time-step prep
    simulation_tick_aggregator()  // sim @ 0x67F430 (throttled by mtr-asi)
    sub_609B90()
    render_frame_top_level()      // render @ 0x4D22D0
    sub_5908C0()
}
```

Sim runs **before** render in the same iteration. This lets render-path
throttles read a `g_sim_ran_last_iteration` flag set by the sim hook to
decimate at the same cadence — no independent time-gate needed.

## Confirmed hidden sites (now throttled in M1.4)

### 1. `tick_2d_overlay_pass` @ 0x4A9F10 — 2D HUD tween dt-integrator

- Single caller: `render_frame_top_level` @ 0x4D236C.
- Chooses `dt = (unk_71D2A0) ? real_dt : flt_6FFCBC` per call.
- Walks 3 lists of 2D overlay entries and calls `sub_4A8B40(dt)` on each.
- `sub_4A8B40` advances each entry's elapsed time by `dt` and updates the
  tween's progress fraction `*(this+8)`. So at 240 Hz with 0.003 dt, all
  HUD tweens (fades, slide-ins, color animations) play 4× faster.
- **mtr-asi response**: `hk_tick_overlay_pass` skips the function entirely
  when `g_sim_ran_last_iteration == false`. Net effect: HUD tweens advance
  at sim Hz, are drawn at render Hz.

### 2. `render_uv_scroll_tick` @ 0x4C24E0 — UV-scroll integrator

- Was `sub_4C24E0` — renamed in IDB.
- Called from `render_uv_scroll_pass` @ 0x4C4110 in a tight loop over up
  to ~132 entries per render frame.
- Inline `0.003` (NOT via `flt_6FFCBC`): `*(v5+44) += 0.003 * *(v5+52)`
  for U scroll, `*(v5+48) += 0.003 * *(v5+56)` for V scroll, plus a third
  channel at `*(v5+68) += 0.003 * *(v5+72)`.
- These are texture UV scrolls — animated water, fog, clouds, lava layers.
  At 240 Hz they scroll 4× faster.
- **mtr-asi response**: `hk_uv_animator` skips the call when sim was
  decimated this iteration. The parent loop continues running, so the
  draw (`sub_4C3790` later in the loop body) still happens with the
  prior frame's UVs. Net effect: UVs advance at sim Hz, draws at render Hz.

## Why the inner-hook + flag pattern (vs per-call time-gate)

Both render-path sites fire many times per render frame:
- overlay_pass walks 3 lists of N entries each
- uv_scroll_tick is invoked ~132 times in a single tight loop

A naive per-call time-gate (`if (now - last < 1/target_hz) return 0`)
would let only the first call through and skip the rest, corrupting the
per-frame batch semantics. Tying decimation to the sim aggregator's
decision (one boolean per render frame) gives clean "all or none per
frame" behaviour at the same effective Hz — which matches the visual
intent (HUD tweens and UVs advance together, on the same cadence as
physics + animation).

## Sites investigated and ruled out

### `render_screen_shake_tick` @ 0x4D1FB0

- Was `sub_4D1FB0` — renamed.
- Single caller: `render_frame_top_level` @ 0x4D2313.
- Reads `flt_6FFCBC` only as a fallback when `unk_725E9C` is set; primary
  path computes real-dt from `game_get_time_ms() - last_call`.
- Output: 4 RGBA bytes at `unk_725E94[0..3]` — a screen-shake / colour
  modulation effect.
- The fallback's effect at 240 Hz is small (a per-frame colour drift),
  and the primary path is already real-dt-correct. Below the bar for
  M1.4 — would only matter if a future test reveals visible drift in
  the affected effect (colour-cycling shaders / damage flashes).

### Functions reading `flt_6FFCBC` from the **sim** chain (already throttled)

Confirmed via xrefs that all of these are reached only from
`simulation_tick_aggregator`:
- `entity_transform_tick` (0x4B9F60) — single sim caller
- `physics_state_machine_tick` (0x4DC150) — sim caller
- `anim_controller_advance` (0x4E2B00) — anim sub-chain
- `anim_update_all_tracks` — sim caller
- `trail_subsystem_tick` (0x4D1D60) — sim caller
- `particle_buckets_sweep_a` (0x4BAA40) — sim caller
- `particle_buckets_sweep_b` (0x4D3E50) — sim caller

Throttling the aggregator at the entry covers all of these by extension —
no per-callee hook needed.

### Static `flt_6FFCBC` xrefs from non-render code

The full xref list of `flt_6FFCBC` has 100+ entries, many in functions
that are not in the per-frame render or sim path (initialization,
configuration, math helpers using 0.003 as an epsilon). These fire at
fixed times (level load, init) or rarely (one-shot) and are not
candidates for throttling.

## What's deliberately NOT static-swept

- **Stolen-byte thunks via `g_securom_thunk_table_base + N`**: not
  encrypted, but reached via indirect calls through a runtime IAT, so
  callgraph walking can miss them. The destination bodies ARE in our
  unpacked dump (per `project_overview.md` the rr01 stub is plain
  aPLib decompression with no lazy decryption). Byte-pattern searches
  cover the whole binary regardless of how calls reach a given
  function, so they do catch these. Left as a runtime watch item: if
  T1–T3 still fail at 240 Hz after this build, a runtime
  read-watchpoint on `flt_6FFCBC` surfaces any consumers we missed.

## Test impact

After deploying the M1.4 build, T1–T3 (jump height / walk speed / anim
timing) should pass at 30/60/120/240 Hz with throttle@60. Visible
secondary correctness gains:
- HUD slide-ins / fades play at correct authored speed.
- Animated textures (water, fog) scroll at correct authored speed.

If anything visibly drifts at 240 Hz that didn't before, it's likely a
site reached only via stolen-byte thunks that we missed in the static pass. Capture with
the detailed log toggle on and bring back the symptom.

## Symbols renamed in IDB

| VA | Old | New |
|-----|-----|-----|
| 0x4C24E0 | `sub_4C24E0` | `render_uv_scroll_tick` |
| 0x4C4110 | `sub_4C4110` | `render_uv_scroll_pass` |
| 0x4D1FB0 | `sub_4D1FB0` | `render_screen_shake_tick` |

`tick_2d_overlay_pass` was already named in a prior session.

## See also

- [`high-fps-decoupling.md`](high-fps-decoupling.md) — architecture
- [`high-fps-decoupling-plan.md`](high-fps-decoupling-plan.md) — engineering plan
- [`docs/decouple-test-protocol.md`](../../docs/decouple-test-protocol.md) — T1–T6 acceptance tests
- `memory/project_decouple_m0_m1_shipped.md` — current shipped state

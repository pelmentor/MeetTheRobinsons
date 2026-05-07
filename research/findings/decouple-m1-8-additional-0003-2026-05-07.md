# M1.8 — additional 0.003 sites (full xref scan)

The earlier "static sweep complete" claim was wrong twice — first M1.7
caught the alt pump's three pre-sim integrators, and now M1.8 catches
four more by paging through the FULL `flt_6FFCBC` xref list (153 total
xrefs, the original query was capped at 100).

## What was missed

The original sweep used `xrefs_to(0x6FFCBC, limit=100)` and got back 100
entries with `more: true`. I never paginated — assumed the un-shown
ones were similar to the shown ones (mostly orphans). Wrong: the
remaining 53 entries included several live engine functions in pump
paths.

## Newly throttled

| VA | Renamed | Pump | Pattern |
|----|---------|------|---------|
| `0x681380` | `timer_wheel_pretick` | alt | first call in alt pump; `timer_wheel_advance(real_dt, 0.003, flag)` decrements per-task timers, fires callbacks |
| `0x596610` | `post_render_entity_sweep` | main | called from `sub_5908C0` POST `render_frame_top_level`; walks entity list, ticks each with 0.003 |
| `0x602F10` | `alt_pump_subsystem_sweep` | alt | gated by `dword_74467C`; walks subsystem list, ticks with 0.003 |
| `0x6582F0` | `alt_pump_pre_sim_audio_sweep` | alt-pre-sim | 3 loops; 3rd loop integrates with 0.003; loops 1-2 are velocity-magnitude damping |

All four hooks gate on `!g_sim_ran_last_iteration` (same pattern as
M1.4 / M1.7). When throttle@60 is engaged at 240 Hz render, each fires
~60×/sec instead of 240×/sec.

## Symptoms this fixes

- **`timer_wheel_pretick`**: scripted timers / scheduled callbacks in
  the alt-pump path (likely cinematic state transitions, special-event
  timers) firing 4× too soon.
- **`post_render_entity_sweep`**: post-render entity book-keeping that
  ticks objects at the wrong cadence in the main pump.
- **`alt_pump_subsystem_sweep`**: subsystem advance gated on
  `dword_74467C` — looks like a debug or world-state subsystem.
- **`alt_pump_pre_sim_audio_sweep`**: audio-related per-frame work
  (3D positional audio updates? speaker damping?) — inferred from the
  velocity-magnitude pattern in loops 1-2.

## Process improvement

Added to the residual-systems doc: when sweeping for hidden time-coupled
work, **always paginate xref queries to completion**. The 100-entry cap
in IDA MCP queries silently truncates. Use `xref_query` with
`count: 500` instead of `xrefs_to` with `limit: 100`.

Also: the lesson "walk both directions from sim_aggregator" remains
correct, but additionally, **walk the engine pumps' own callees** — not
just sim/render. The pumps wrap around the aggregator with their own
pre-sim and post-render hookup of subsystems.

## Renames in IDB

| VA | New |
|----|-----|
| 0x681380 | `timer_wheel_pretick` |
| 0x586F30 | `timer_wheel_advance` |
| 0x596610 | `post_render_entity_sweep` |
| 0x596570 | `post_render_entity_step` |
| 0x602F10 | `alt_pump_subsystem_sweep` |
| 0x6582F0 | `alt_pump_pre_sim_audio_sweep` |

## Telemetry

Tools → Decouple → "M1.8: timer=N post_render=N alt_subsys=N
alt_audio=N" — counts how many times each was decimated. Each can be
non-zero independently based on which pump is active and which gating
flags are set.

## Files touched

| File | Change |
|------|--------|
| `src/mtr-asi/include/mtr/sim_decouple.h` | 4 new skip-counter accessors |
| `src/mtr-asi/src/sim_decouple.cpp` | 4 new MinHook detours + state |
| `src/mtr-asi/src/menu.cpp` | M1.8 telemetry line |

## Total decouple coverage now

| Pump path | Throttled functions |
|-----------|---------------------|
| Main pump | sim_aggregator (covers physics/anim/particle/trail/entity), pathcam_pretick, post_render_entity_sweep |
| Alt pump | sim_aggregator, timer_wheel_pretick, alt_pump_pre_sim_audio_sweep, chain_physics_pass, managed_object_list_tick, wave_grid_tick, alt_pump_subsystem_sweep |
| Render path (both pumps) | tick_2d_overlay_pass, render_uv_scroll_tick |
| Engine globals | dword_6FFCA4 dt-correction PRE-orig-sim |

That's 12 throttle hooks + 1 dt-correction patch.

## See also

- [`decouple-m1-7-alt-pump-2026-05-07.md`](decouple-m1-7-alt-pump-2026-05-07.md) — alt pump discovery
- [`decouple-hidden-0003-sites-2026-05-07.md`](decouple-hidden-0003-sites-2026-05-07.md) — original M1.4 sweep (now superseded twice)
- `memory/project_decouple_m1_8_more_0003.md` — pointer

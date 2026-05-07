# M1.6 — engine frame-dt correction under throttle

Discovered post-M5: the engine's frame-dt global at `0x6FFCA4` is
poisoned when sim is throttled. This patch corrects it.

## The bug

Engine pump order:
```
sub_57A2C0()                  // dt prep — writes dword_6FFCA4 = ms since last call
simulation_tick_aggregator()  // sim
sub_609B90()
render_frame_top_level()      // render
```

`sub_57A2C0` runs every pump iteration regardless of throttle. At 240 Hz
render, it writes `dword_6FFCA4 = ~4 ms` every iteration.

Several systems INSIDE `simulation_tick_aggregator` and the render
frame read `dword_6FFCA4` as their dt:

- `sub_60EED0(dword_6FFCA4 * 0.001)` — subscriber tick list at the END
  of `simulation_tick_aggregator`. Walks `dword_744C4C` linked list
  calling `vtable[2](this, dt)` on each priority-qualified subscriber.
  Catches background timers, fades, shader-uniform animators, etc.
- `tick_2d_overlay_pass` — when its `unk_71D2A0` flag is set, advances
  HUD tweens via `dword_6FFCA4 * 0.001` instead of the 0.003 fallback.
- `particle_update_tick` — when entry flags `& 0x2000000` is set, reads
  `dword_724DFC[1]` which `particle_buckets_sweep_a` populates from
  `dword_6FFCA4 * 0.001`.

When sim is throttled to 60 Hz with render at 240 Hz, sim_aggregator
fires every 4th render frame. On those frames `dword_6FFCA4 = 4 ms`
even though `~16.7 ms` of real time has passed since the last sim tick.
Time-based subsystems thus tick at **25% of intended speed** — fades
take 4× longer, music-sync animators drift, shader-uniform clocks
crawl.

## The fix

Pre-orig-sim, when throttle is engaged, overwrite `dword_6FFCA4` with
the real elapsed milliseconds since our last actual sim run:

```cpp
if (effective_is_throttling_impl()) {
    const uint64_t now = qpc_now();
    const uint64_t prev = g_last_actual_sim_qpc;
    if (prev != 0) {
        uint64_t dt_ms_64 = ((now - prev) * 1000) / qpc_freq;
        if (dt_ms_64 > 100) dt_ms_64 = 100;   // cap pause / load spikes
        if (dt_ms_64 > 0) {
            *(volatile uint32_t*)0x6FFCA4 = (uint32_t)dt_ms_64;
            g_dt_corrections_applied++;
        }
    }
    g_last_actual_sim_qpc = now;
}
int rc = g_orig_sim_aggregator(this_, 0);
```

When throttle is OFF, no write — sub_57A2C0's natural value stays.

## Why no fence / restore

`dword_6FFCA4` is the engine's "frame dt", read by render-path
consumers (`tick_2d_overlay_pass`) AFTER sim returns. We WANT them to
see the corrected dt (since `tick_2d_overlay_pass` is throttle-gated to
sim-cadence too, its expected dt is "ms since last sim", same as ours).

The next pump iteration's `sub_57A2C0` overwrites the global with a
fresh small value (~4 ms) before the next sim tick. So our write
"persists" only for the current render frame, which is exactly the
window we care about.

## Edge cases handled

- **First sim tick after install**: `g_last_actual_sim_qpc == 0` → skip
  the write; the engine's natural dt (4 ms) propagates. After the
  first tick we have a baseline.
- **Pause / load spike**: cap dt at 100 ms (10 Hz minimum apparent
  rate). Beyond that, the engine itself is misbehaving and a longer dt
  would compound the disturbance.
- **Throttle toggled OFF mid-frame**: next pump iteration skips the
  write; render-path readers get sub_57A2C0's natural dt. No restore
  needed because there's nothing to restore.

## Telemetry

`mtr::sim_decouple::dt_corrections_applied()` cumulative counter,
shown in Tools → Decouple → "Frame-dt corrections: N (M1.6)". At 240 Hz
with throttle@60, expect this to increment ~60× per second.

## Why this wasn't in the original plan

The plan §M1.4 hidden-site sweep was scoped to "find functions that
read `flt_6FFCBC` outside the aggregator chain". `dword_6FFCA4` is a
DIFFERENT global (real-dt in ms vs. fixed 0.003 step). The sweep
caught the constant-readers but not the dt-readers because they're
dt-correct under normal authored flow — only our throttle introduces
the dt-vs-call-cadence mismatch.

## Validation

Symptoms that would surface a missed M1.6 fix:
- HUD fade-ins / slide-outs play 4× slower at 240 Hz vs 60 Hz with
  throttle on (unk_71D2A0 path)
- Background music sync drifts noticeably over time
- Lava / portal shader uniforms appear "stuck" at 240 Hz

Symptoms that confirm M1.6 working:
- Telemetry counter increments matching sim rate
- Above effects play at native authored speed regardless of render rate

## Files touched

| File | Change |
|------|--------|
| `src/mtr-asi/include/mtr/sim_decouple.h` | adds `dt_corrections_applied()` |
| `src/mtr-asi/src/sim_decouple.cpp` | `g_last_actual_sim_qpc`, `g_dt_corrections_applied`, dt-overwrite block in `hk_sim_aggregator` |
| `src/mtr-asi/src/menu.cpp` | telemetry line in Decouple section |

## See also

- [`high-fps-decoupling-plan.md`](high-fps-decoupling-plan.md) §M1.4 — original sweep scope
- `decouple-hidden-0003-sites-2026-05-07.md` — the constant-reader sweep
- `memory/project_decouple_m1_6_dt_correction.md` — short pointer

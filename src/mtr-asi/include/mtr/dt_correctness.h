// Engine-wide dt-correctness for the hardcoded `flt_6FFCBC = 0.003`
// consumers (~150 of them — physics, animation, particles, pathcam,
// chain physics, screen shake, UV scroll, HUD tweens, etc.).
//
// See research/findings/dt-correctness-root-cause-2026-05-07.md for the
// full investigation. Short version: the engine integrates against a
// single global float `flt_6FFCBC = 0.003` for every fixed-step
// subsystem. At any pump rate other than ~333 Hz (= 1 / 0.003),
// everything is mis-scaled.
//
// This module writes `flt_6FFCBC` to the **real elapsed time per
// consumer phase** so all 150+ subsystems automatically become
// framerate-independent. Two write sites cover the cases:
//
//   1. interp.cpp's camera_apply_all_active PRE — calls
//      write_for_render_frame() so render-path consumers (PathCam
//      spring, UV scroll, HUD tweens, screen shake) integrate at
//      real_render_dt.
//   2. sim_decouple's hk_sim_aggregator PRE — calls write_for_sim_run()
//      so sim-path consumers (entity_transform_tick,
//      physics_state_machine_tick, anim_update_all_tracks) integrate
//      at real_sim_dt.
//
// We piggyback on the existing camera_apply / sim_aggregator hooks so
// no extra detours are needed; this avoids the __usercall calling
// convention pitfalls of hooking the engine pump entry directly.
//
// Toggle is default ON. When OFF, flt_6FFCBC reverts to the engine
// default 0.003 and the engine behaves as ship-vanilla.

#pragma once

#include <cstdint>

namespace mtr::dt_correctness {

// Install MinHook detours on parameter-dt particle paths
// (sub_4F45F0 and sub_47E6B0). These functions take dt as a function
// argument rather than reading flt_6FFCBC, so dt-correctness can't
// reach them via the global write. Hooking them lets us scale their
// dt according to the current time-scale mode.
void install();

// Toggle. Default ON.
bool enabled();
void set_enabled(bool on);

// Time-scale mode: how does game speed relate to user's chosen sim_hz?
//
// RealTime: world plays at 1.0 sec/sec at any sim_hz. sim_hz is a pure
//   FIDELITY dial — higher Hz = finer game-logic resolution but no
//   speed change. Default and recommended.
//
// SlowMoAtLowSim: world plays at sim_hz/60 speed. sim_hz=60 is normal,
//   sim_hz=15 is quarter-speed (cinematic slow-mo), sim_hz=120 is 2x
//   fast-forward. Particles, animations, and the camera spring all
//   scale with sim_hz. Useful for cinematic effects or stress-testing.
//
// VisualLockToSim: visual-path consumers (particle integrator dt,
//   render-path flt_6FFCBC + dword_6FFCA4) advance only on render
//   frames where sim actually ticked, and pass dt=0 between sim ticks.
//   At sim_hz=15, glow/sprite-UV/particles update visibly in 15
//   discrete steps per second, while render still draws at 240 Hz. Use
//   for low-fidelity testing and "what does this look like at N Hz?"
//   experiments. Camera interp + view smoothing are unaffected (those
//   read real-render-dt independently of flt_6FFCBC).
//
// Off: revert to vanilla engine timing (everything reads 0.003 fixed
//   step). Effectively disables dt-correctness entirely.
enum class TimeScale {
    RealTime        = 0,
    SlowMoAtLowSim  = 1,
    Off             = 2,
    VisualLockToSim = 3,
};

TimeScale time_scale();
void set_time_scale(TimeScale ts);

// Effective scaling factor applied to "render-path" dt sources (the
// camera_apply hook's flt_6FFCBC write, and the parameter-dt particle
// hooks). RealTime → 1.0, SlowMoAtLowSim → sim_hz/60.0, Off → 1.0.
float render_dt_scale();

// Live-tunable safety clamps on the dt value. Stops physics from
// blowing up at very low pump rates (long stalls / level loads), and
// avoids divide-by-near-zero in spring calculations at very high pump
// rates.
//
// Defaults: min = 0.0005s (= 2000 Hz cap), max = 0.0667s (= 15 Hz floor).
float min_dt();
void  set_min_dt(float v);
float max_dt();
void  set_max_dt(float v);

// Particle / trail "feel" mix. Lerps the dt particle paths see between
//   0.0 = engine-vanilla 0.003 fixed step (game-authored content speed —
//         particles decay at sim_hz × 0.003 sec/sec, matching the
//         original "looks correct" feel the user sees with dt-correctness
//         OFF)
//   1.0 = strict real-time dt (matches dt-correctness's intent: 1.0 sec
//         of decay per 1.0 sec wall clock, regardless of sim_hz)
//
// Affects ONLY trail_subsystem_tick + particle_buckets_sweep_a/b —
// physics / animation / entity transforms keep the corrected real-time
// dt, so high refresh rates remain framerate-independent there.
//
// Default 0.0 (vanilla particle feel) because the engine's particle
// content was authored against 0.003 and users expect that speed.
// Crank to 1.0 if you want particles to decay at "true" authored
// real-time, useful for stress-testing or cinematic slow-mo.
float particle_feel_mix();
void  set_particle_feel_mix(float v);   // clamped to [0.0, 1.0]

// Diagnostics.
double last_render_dt();
double last_sim_dt();
double last_alt_pump_dt();
uint64_t render_writes();
uint64_t sim_writes();
uint64_t alt_pump_writes();
uint64_t particle_dt_overrides();   // count of sub_4F45F0 hits while dtc enabled

// Called at the top of each render-frame's camera_apply_all_active
// hook (interp.cpp). Tracks render-frame-to-render-frame elapsed time
// internally; writes flt_6FFCBC = clamped(elapsed) so render-path
// consumers integrate at real-render-dt for this frame.
void write_for_render_frame();

// Called at the top of each sim aggregator that's about to actually
// run orig sim (sim_decouple.cpp), with the real elapsed seconds
// since the last actual sim run (NOT since the last pump iteration —
// those differ when sim is throttled). Writes flt_6FFCBC =
// clamped(elapsed_seconds) so sim's children integrate at correct
// real-time rate.
void write_for_sim_run(double elapsed_seconds);

// Called at the top of each alt-pump iteration (engine_pump_alt @
// 0x682010, via the timer_wheel_pretick hook in sim_decouple). Tracks
// alt-pump iteration cadence internally; writes flt_6FFCBC =
// clamped(real_alt_pump_dt) so alt-pump consumers (wave_grid_tick,
// chain_physics, managed_object_list, alt_subsys_sweep, alt_audio_sweep,
// timer_wheel itself) integrate at correct rate.
void write_for_alt_pump();

// Periodic structured log emit. Designed to be called from EndScene
// every render frame; internally rate-limits to ~one log line per 2
// seconds. Always-on (independent of `enabled()`) so the user can
// capture a "dt-correctness off" baseline by toggling and then a
// "dt-correctness on" sample for comparison in the same run. Emits a
// single self-describing line tagged `[dtc-snap]` so downstream log
// review can grep it cleanly.
void tick_snapshot_log();

} // namespace mtr::dt_correctness

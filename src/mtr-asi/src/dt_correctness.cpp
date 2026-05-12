// dt-correctness — write flt_6FFCBC = real_elapsed_time so all 150+
// subsystems that integrate with hardcoded 0.003 become
// framerate-independent.
//
// Hook strategy: piggyback on existing hooks in interp.cpp
// (camera_apply_all_active) and sim_decouple.cpp (sim_aggregator).
// No new MinHook detours; we just expose write_* APIs that those TUs
// call before their orig runs.

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>

#include "mtr/dt_correctness.h"
#include "mtr/sim_decouple.h"

namespace mtr::log         { void info(const char* fmt, ...); }
namespace mtr::screen_push { bool current_top_name(char* out, size_t out_size); }
namespace mtr::sim_decouple {
    bool sim_ticked_this_render_frame();
}
namespace mtr::windowmode  {
    bool enabled();
    unsigned long long create_device_rewrites();
    unsigned long long reset_rewrites();
    unsigned long long change_display_settings_blocks();
}

namespace mtr::dt_correctness {

namespace {

// Universal hardcoded dt global. Verified: bytes 0xA6 0x9B 0x44 0x3B
// (= 0.003 little-endian) appear at exactly one location in the binary
// — this address. Every "0.003" reference in the disassembly is an
// `fld dword ptr [flt_6FFCBC]` load. Patching this one global at
// runtime controls the dt of every consumer simultaneously.
constexpr uintptr_t kFltConstVA = 0x006FFCBC;

// Frame-dt-ms global. Read by sub_60EED0 sim subscribers (audio fades,
// music sync, shader uniforms) and by tick_2d_overlay_pass when its
// HUD-element flag at unk_71D2A0 is set. The engine only writes this
// at one (unused-init) site at 0x6A3F60 — so without our patches it
// stays at the static initial value of 3 ms. M1.6 used to write it
// only at sim time; we extend that to render time too so HUD elements
// using dword_6FFCA4 see real_render_dt instead of stale 67ms.
constexpr uintptr_t kFrameDtMsGlobalVA = 0x006FFCA4;

std::atomic<float> g_min_dt{0.0005f};   // 2000 Hz cap
std::atomic<float> g_max_dt{0.0667f};   // 15 Hz floor

std::atomic<bool> g_enabled{true};
std::atomic<int>  g_time_scale{static_cast<int>(TimeScale::RealTime)};

std::atomic<double>   g_last_render_dt{0.0};
std::atomic<double>   g_last_sim_dt{0.0};
std::atomic<uint64_t> g_render_writes{0};
std::atomic<uint64_t> g_sim_writes{0};
std::atomic<uint64_t> g_particle_overrides{0};

// QPC plumbing for per-render-frame dt measurement. We don't trust
// the engine's dword_6FFCA4 because it's smoothed/clamped/written by
// engine code we'd be racing.
uint64_t g_qpc_freq = 0;
std::atomic<uint64_t> g_last_render_qpc{0};
std::atomic<uint64_t> g_last_alt_pump_qpc{0};
std::atomic<double>   g_last_alt_pump_dt{0.0};
std::atomic<uint64_t> g_alt_pump_writes{0};

// Periodic snapshot log state. Always-on (independent of g_enabled) so
// the user can capture an OFF baseline + ON sample in one play session.
// Emits at most one [dtc-snap] line per kSnapIntervalSec wall-clock
// seconds — cheap and trivial to grep / parse.
constexpr double kSnapIntervalSec = 2.0;
std::atomic<uint64_t> g_snap_last_qpc{0};
std::atomic<uint64_t> g_snap_render_writes_at{0};
std::atomic<uint64_t> g_snap_sim_writes_at{0};
std::atomic<uint64_t> g_snap_alt_writes_at{0};
std::atomic<uint64_t> g_snap_particle_overrides_at{0};

uint64_t qpc_now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

void ensure_qpc_freq() {
    if (g_qpc_freq) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = static_cast<uint64_t>(f.QuadPart);
}

float clamp_dt(float v) {
    const float lo = g_min_dt.load(std::memory_order_relaxed);
    const float hi = g_max_dt.load(std::memory_order_relaxed);
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void write_flt_const(float v) {
    *reinterpret_cast<volatile float*>(kFltConstVA) = v;
}

void write_dword_6ffca4_ms(uint32_t ms) {
    *reinterpret_cast<volatile uint32_t*>(kFrameDtMsGlobalVA) = ms;
}

float compute_render_dt_scale() {
    if (!g_enabled.load(std::memory_order_relaxed)) return 1.0f;
    const TimeScale ts = static_cast<TimeScale>(g_time_scale.load(std::memory_order_relaxed));
    if (ts == TimeScale::SlowMoAtLowSim) {
        const int sim_hz = mtr::sim_decouple::target_hz();
        if (sim_hz <= 0) return 1.0f;
        return static_cast<float>(sim_hz) / 60.0f;
    }
    return 1.0f;   // RealTime, Off
}

// Particle integrator hook. sub_4F45F0 takes dt as float param (a2)
// rather than reading flt_6FFCBC. We override `a2` based on time-scale
// mode so particle pos/vel integration follows the same speed contract
// as the rest of the engine.
//
// Hook is __thiscall (this in ECX, a2 on stack). MSVC __fastcall with
// dummy edx slot provides the matching ABI.
constexpr uintptr_t kSub4F45F0VA = 0x004F45F0;

using PFN_Sub4F45F0 = char (__fastcall*)(void* this_, int /*edx*/, float a2);
PFN_Sub4F45F0 g_orig_sub_4f45f0 = nullptr;

// Particle/trail feel mix (vanilla 0.003 ↔ real-time real_sim_dt).
// 0.0 = pure vanilla 0.003 (engine-authored decay rate, what users see
//       when dt-correctness is OFF). 1.0 = pure real-time. Default 0.0
//       — preserves authored particle feel even when dt-correctness is
//       ON for physics / animation.
std::atomic<float> g_particle_feel_mix{0.0f};
constexpr float kEngineVanillaDt = 0.003f;

// Particle-path entry hooks. These three are the ONLY sim_aggregator
// children that run particle / trail update loops, and each reads
// flt_6FFCBC (= g_engine_universal_dt_003) to drive lifetime decay +
// position integration. We wrap each with save / scale / orig / restore
// so the OTHER sim children (physics, anim, entity transforms) see the
// real flt_6FFCBC at their normal value.
constexpr uintptr_t kTrailSubsystemTickVA   = 0x004D1D60;
constexpr uintptr_t kParticleBucketsSweepAVA = 0x004BAA40;
constexpr uintptr_t kParticleBucketsSweepBVA = 0x004D3E50;

using PFN_VoidThiscall = void (__fastcall*)(void* this_, int /*edx*/);
using PFN_VoidCdecl    = void (__cdecl*)();
PFN_VoidCdecl    g_orig_trail_subsystem_tick    = nullptr;
PFN_VoidThiscall g_orig_particle_buckets_sweep_a = nullptr;
PFN_VoidThiscall g_orig_particle_buckets_sweep_b = nullptr;

// Apply the particle-feel mix around an orig call. Lerps flt_6FFCBC
// between engine-vanilla 0.003 and the current real-time-corrected
// value, then runs orig, then restores. Save / restore makes it nest-
// safe and a no-op when dt-correctness is disabled (or mix=1.0 AND dtc
// is on, which is the "real-time" extreme — saved == new in that case).
inline void with_mixed_flt(float mix, auto&& body) {
    const float saved = *reinterpret_cast<volatile float*>(kFltConstVA);
    if (g_enabled.load(std::memory_order_relaxed)) {
        // mix=0 → vanilla 0.003. mix=1 → saved (real-time).
        const float mixed = kEngineVanillaDt + (saved - kEngineVanillaDt) * mix;
        *reinterpret_cast<volatile float*>(kFltConstVA) = mixed;
    }
    body();
    *reinterpret_cast<volatile float*>(kFltConstVA) = saved;
}

void __cdecl hk_trail_subsystem_tick() {
    const float mix = g_particle_feel_mix.load(std::memory_order_relaxed);
    with_mixed_flt(mix, [&]() { g_orig_trail_subsystem_tick(); });
}

void __fastcall hk_particle_buckets_sweep_a(void* this_, int /*edx*/) {
    const float mix = g_particle_feel_mix.load(std::memory_order_relaxed);
    with_mixed_flt(mix, [&]() { g_orig_particle_buckets_sweep_a(this_, 0); });
}

void __fastcall hk_particle_buckets_sweep_b(void* this_, int /*edx*/) {
    const float mix = g_particle_feel_mix.load(std::memory_order_relaxed);
    with_mixed_flt(mix, [&]() { g_orig_particle_buckets_sweep_b(this_, 0); });
}

char __fastcall hk_sub_4F45F0(void* this_, int /*edx*/, float a2) {
    if (g_enabled.load(std::memory_order_relaxed)) {
        const TimeScale ts = static_cast<TimeScale>(
            g_time_scale.load(std::memory_order_relaxed));

        // VisualLockToSim: pass dt=0 on render frames where sim didn't
        // tick. Particles freeze between sim boundaries, advance one
        // engine-dt step on each sim-tick frame.
        if (ts == TimeScale::VisualLockToSim) {
            if (!mtr::sim_decouple::sim_ticked_this_render_frame()) {
                g_particle_overrides.fetch_add(1, std::memory_order_relaxed);
                return g_orig_sub_4f45f0(this_, 0, 0.0f);
            }
            // Sim ticked this frame → pass through engine's authored dt
            return g_orig_sub_4f45f0(this_, 0, a2);
        }

        const float scale = compute_render_dt_scale();
        if (scale != 1.0f) {
            g_particle_overrides.fetch_add(1, std::memory_order_relaxed);
            return g_orig_sub_4f45f0(this_, 0, a2 * scale);
        }
    }
    return g_orig_sub_4f45f0(this_, 0, a2);
}

} // namespace

void install() {
    ensure_qpc_freq();

    // Hook the parameter-dt particle integrator so we can scale
    // particles' speed under SlowMoAtLowSim mode. RealTime mode just
    // passes through, so the hook is a no-op there.
    void* va = reinterpret_cast<void*>(kSub4F45F0VA);
    if (MH_CreateHook(va, reinterpret_cast<void*>(&hk_sub_4F45F0),
                      reinterpret_cast<void**>(&g_orig_sub_4f45f0)) != MH_OK) {
        mtr::log::info("dt_correctness: MH_CreateHook(sub_4F45F0 @%p) failed", va);
    } else if (MH_EnableHook(va) != MH_OK) {
        mtr::log::info("dt_correctness: MH_EnableHook(sub_4F45F0) failed");
    } else {
        mtr::log::info("dt_correctness: hooked sub_4F45F0 @%p (particle integrator dt override)", va);
    }

    // Particle-path "feel" hooks. Wrap the three sim_aggregator children
    // that run particle / trail update loops. PRE-orig: scale flt_6FFCBC
    // by particle_feel_scale. POST-orig: restore. Lets users dial back
    // particle lifetime decay rate to vanilla-60Hz feel (~0.18×) without
    // affecting physics / animation / entity transforms (they read flt
    // BEFORE these hooks fire).
    auto try_hook = [](uintptr_t va, void* detour, void** orig_out, const char* name) {
        void* p = reinterpret_cast<void*>(va);
        if (MH_CreateHook(p, detour, orig_out) != MH_OK) {
            mtr::log::info("dt_correctness: MH_CreateHook(%s @%p) failed", name, p);
        } else if (MH_EnableHook(p) != MH_OK) {
            mtr::log::info("dt_correctness: MH_EnableHook(%s) failed", name);
        } else {
            mtr::log::info("dt_correctness: hooked %s at %p", name, p);
        }
    };
    try_hook(kTrailSubsystemTickVA,
             reinterpret_cast<void*>(&hk_trail_subsystem_tick),
             reinterpret_cast<void**>(&g_orig_trail_subsystem_tick),
             "trail_subsystem_tick");
    try_hook(kParticleBucketsSweepAVA,
             reinterpret_cast<void*>(&hk_particle_buckets_sweep_a),
             reinterpret_cast<void**>(&g_orig_particle_buckets_sweep_a),
             "particle_buckets_sweep_a");
    try_hook(kParticleBucketsSweepBVA,
             reinterpret_cast<void*>(&hk_particle_buckets_sweep_b),
             reinterpret_cast<void**>(&g_orig_particle_buckets_sweep_b),
             "particle_buckets_sweep_b");

    mtr::log::info("dt_correctness: enabled — flt_6FFCBC + dword_6FFCA4 will be"
                   " set to real dt from camera_apply / sim_aggregator hooks");
}

bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

void set_enabled(bool on) {
    const bool prev = g_enabled.exchange(on, std::memory_order_relaxed);
    if (prev != on) {
        mtr::log::info("dt_correctness: %s", on ? "enabled" : "disabled");
        if (!on) {
            // Restore engine-authored constant so consumers running
            // between this point and the next pump iteration see what
            // the engine shipped with.
            write_flt_const(0.003f);
        }
    }
}

float min_dt() { return g_min_dt.load(std::memory_order_relaxed); }
void  set_min_dt(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_min_dt.store(v, std::memory_order_relaxed);
}

float max_dt() { return g_max_dt.load(std::memory_order_relaxed); }
void  set_max_dt(float v) {
    if (v < 0.001f) v = 0.001f;
    if (v > 1.0f)   v = 1.0f;
    g_max_dt.store(v, std::memory_order_relaxed);
}

float particle_feel_mix() {
    return g_particle_feel_mix.load(std::memory_order_relaxed);
}
void set_particle_feel_mix(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_particle_feel_mix.store(v, std::memory_order_relaxed);
}

double last_render_dt() { return g_last_render_dt.load(std::memory_order_relaxed); }
double last_sim_dt()    { return g_last_sim_dt.load(std::memory_order_relaxed); }
double last_alt_pump_dt() { return g_last_alt_pump_dt.load(std::memory_order_relaxed); }
uint64_t render_writes() { return g_render_writes.load(std::memory_order_relaxed); }
uint64_t sim_writes()    { return g_sim_writes.load(std::memory_order_relaxed); }
uint64_t alt_pump_writes() { return g_alt_pump_writes.load(std::memory_order_relaxed); }
uint64_t particle_dt_overrides() { return g_particle_overrides.load(std::memory_order_relaxed); }

TimeScale time_scale() {
    return static_cast<TimeScale>(g_time_scale.load(std::memory_order_relaxed));
}

void set_time_scale(TimeScale ts) {
    const int prev = g_time_scale.exchange(static_cast<int>(ts), std::memory_order_relaxed);
    if (prev != static_cast<int>(ts)) {
        const char* name = (ts == TimeScale::RealTime)        ? "RealTime"
                         : (ts == TimeScale::SlowMoAtLowSim)  ? "SlowMoAtLowSim"
                         : (ts == TimeScale::VisualLockToSim) ? "VisualLockToSim"
                                                              : "Off";
        mtr::log::info("dt_correctness: time_scale -> %s", name);
    }
}

float render_dt_scale() {
    return compute_render_dt_scale();
}

void write_for_render_frame() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    ensure_qpc_freq();
    if (!g_qpc_freq) return;
    const uint64_t now = qpc_now();
    const uint64_t prev = g_last_render_qpc.load(std::memory_order_relaxed);
    g_last_render_qpc.store(now, std::memory_order_relaxed);
    if (prev == 0) return;   // first call: no previous timestamp to compare

    // VisualLockToSim: render-path flt write proceeds NORMALLY (with
    // real_render_dt), because the engine's main-menu camera spline,
    // PathCam spring, fade timers and similar render-path consumers
    // all read flt for time-progress and would freeze if we zeroed it
    // between sim ticks. The "choppy at sim_hz" effect is achieved
    // instead by re-enabling sim_decouple's per-subsystem throttles
    // (UV animator, overlay, wave grid, chain physics, etc.) which
    // decimate state-mutation calls to sim rate without touching flt
    // for camera/cinematic consumers. Particle integrator (sub_4F45F0)
    // is also gated on the sim-tick edge in hk_sub_4F45F0 below.
    //
    // Falls through to the standard real_render_dt write path below.

    const double dt_sec_raw = static_cast<double>(now - prev) /
                              static_cast<double>(g_qpc_freq);
    // Apply time-scale to render-path consumers (PathCam spring, HUD,
    // UV scroll, screen shake). RealTime → unchanged. SlowMoAtLowSim →
    // multiplied by sim_hz/60 so the engine's render-path animations
    // slow with the sim_hz dial, matching the sim-path scaling.
    const float scale = compute_render_dt_scale();
    const float dt_clamped = clamp_dt(static_cast<float>(dt_sec_raw * scale));
    write_flt_const(dt_clamped);

    // dword_6FFCA4 (frame-dt in ms) — read by tick_2d_overlay_pass for
    // HUD elements with unk_71D2A0 flag, and by particle_buckets_sweep_a
    // when storing the "real_dt" slot of the particle context. Without
    // a render-path update it stays stale between sim ticks (M1.6 only
    // updated at sim time). Now refreshed every render frame.
    const double dt_ms_scaled = (dt_sec_raw * scale) * 1000.0;
    const uint32_t dt_ms = (dt_ms_scaled > 100.0) ? 100u
                         : (dt_ms_scaled < 1.0)   ? 1u
                         : static_cast<uint32_t>(dt_ms_scaled);
    write_dword_6ffca4_ms(dt_ms);

    g_last_render_dt.store(dt_clamped, std::memory_order_relaxed);
    g_render_writes.fetch_add(1, std::memory_order_relaxed);
}

void write_for_alt_pump() {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    ensure_qpc_freq();
    if (!g_qpc_freq) return;
    const uint64_t now = qpc_now();
    const uint64_t prev = g_last_alt_pump_qpc.load(std::memory_order_relaxed);
    g_last_alt_pump_qpc.store(now, std::memory_order_relaxed);
    if (prev == 0) return;

    // VisualLockToSim: alt-pump consumers (wave_grid, chain_physics,
    // managed_object, timer_wheel, etc.) are throttled by re-enabling
    // sim_decouple's per-subsystem throttle (see subsystem_throttles_active
    // override below), so they skip on non-sim frames. flt itself stays
    // at real_alt_pump_dt so render-path consumers reading it (camera,
    // fade timers) keep working. Falls through to standard write path.

    const double dt_sec_raw = static_cast<double>(now - prev) /
                              static_cast<double>(g_qpc_freq);
    const float scale = compute_render_dt_scale();
    const float dt_clamped = clamp_dt(static_cast<float>(dt_sec_raw * scale));
    write_flt_const(dt_clamped);
    g_last_alt_pump_dt.store(dt_clamped, std::memory_order_relaxed);
    g_alt_pump_writes.fetch_add(1, std::memory_order_relaxed);
}

// Periodic structured-line snapshot. Gated on wall-clock (kSnapIntervalSec)
// so the log doesn't drown in lines at 240 Hz. Captures everything I need
// to diagnose dt-correctness behavior + Phase B windowmode counters from
// the log alone (no need for live debug session):
//
//   dtc=ON/OFF                — current toggle state
//   ts=...                    — TimeScale (RealTime / SlowMoAtLowSim / Off)
//   sim_hz=N throttle=0/1     — sim_decouple state
//   minigame=0/1              — auto-disable detected mini-game?
//   screen=...                — current top screen name (game context)
//   render(rate dt total)     — render-path write rate, last dt, cumulative count
//   sim(rate dt total)        — sim-path same
//   alt(rate dt total)        — alt-pump same
//   particle_dt_overrides     — sub_4F45F0 overrides (only fires in SlowMoAtLowSim)
//   flt_6FFCBC                — current value of the universal hardcoded dt
//   dword_6FFCA4              — current value of the frame-dt-ms global
//   wm(create reset cds)      — windowmode rewrite counters (Phase B)
//
// User test recipe (so the log explains itself):
//   (1) Launch game. Open Insert menu.
//   (2) Set Sim Hz = 15, dt-correctness = OFF. Walk into a particle
//       scene (campfire / water / sparkle). Wait ~10s.
//   (3) Set dt-correctness = ON, TimeScale = RealTime. Wait ~10s.
//   (4) Set TimeScale = SlowMoAtLowSim. Wait ~10s.
//   (5) Quit.
// Resulting log will have ~15 [dtc-snap] lines, with phase changes
// visible in the dtc / ts fields.
void tick_snapshot_log() {
    ensure_qpc_freq();
    if (!g_qpc_freq) return;

    const uint64_t now  = qpc_now();
    const uint64_t last = g_snap_last_qpc.load(std::memory_order_relaxed);
    if (last == 0) {
        g_snap_last_qpc.store(now, std::memory_order_relaxed);
        g_snap_render_writes_at.store(g_render_writes.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_snap_sim_writes_at.store(g_sim_writes.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_snap_alt_writes_at.store(g_alt_pump_writes.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_snap_particle_overrides_at.store(g_particle_overrides.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return;
    }
    const double sec_since = static_cast<double>(now - last) /
                             static_cast<double>(g_qpc_freq);
    if (sec_since < kSnapIntervalSec) return;

    const uint64_t r_now = g_render_writes.load(std::memory_order_relaxed);
    const uint64_t s_now = g_sim_writes.load(std::memory_order_relaxed);
    const uint64_t a_now = g_alt_pump_writes.load(std::memory_order_relaxed);
    const uint64_t p_now = g_particle_overrides.load(std::memory_order_relaxed);
    const uint64_t r_dlt = r_now - g_snap_render_writes_at.load(std::memory_order_relaxed);
    const uint64_t s_dlt = s_now - g_snap_sim_writes_at.load(std::memory_order_relaxed);
    const uint64_t a_dlt = a_now - g_snap_alt_writes_at.load(std::memory_order_relaxed);
    const uint64_t p_dlt = p_now - g_snap_particle_overrides_at.load(std::memory_order_relaxed);

    g_snap_last_qpc.store(now, std::memory_order_relaxed);
    g_snap_render_writes_at.store(r_now, std::memory_order_relaxed);
    g_snap_sim_writes_at.store(s_now, std::memory_order_relaxed);
    g_snap_alt_writes_at.store(a_now, std::memory_order_relaxed);
    g_snap_particle_overrides_at.store(p_now, std::memory_order_relaxed);

    char top[64] = {0};
    mtr::screen_push::current_top_name(top, sizeof(top));

    const float    flt_now = *reinterpret_cast<volatile float*>(kFltConstVA);
    const uint32_t ms_now  = *reinterpret_cast<volatile uint32_t*>(kFrameDtMsGlobalVA);

    const TimeScale ts = static_cast<TimeScale>(g_time_scale.load(std::memory_order_relaxed));
    const char* ts_name = (ts == TimeScale::RealTime)        ? "RealTime"
                        : (ts == TimeScale::SlowMoAtLowSim)  ? "SlowMoAtLowSim"
                        : (ts == TimeScale::VisualLockToSim) ? "VisualLockToSim"
                                                             : "Off";

    const int  sim_hz       = mtr::sim_decouple::target_hz();
    const bool throttling   = mtr::sim_decouple::effective_is_throttling();
    const bool minigame     = mtr::sim_decouple::minigame_detected();
    const bool sim_this_frm = mtr::sim_decouple::sim_ticked_this_render_frame();

    const unsigned long long wm_create = mtr::windowmode::create_device_rewrites();
    const unsigned long long wm_reset  = mtr::windowmode::reset_rewrites();
    const unsigned long long wm_cds    = mtr::windowmode::change_display_settings_blocks();
    const bool wm_on = mtr::windowmode::enabled();

    mtr::log::info(
        "[dtc-snap] win=%.2fs dtc=%s ts=%s sim_hz=%d throttle=%d minigame=%d "
        "sim_this_frm=%d screen=%s "
        "| render(rate=%.1f/s dt=%.4fms tot=%llu) "
        "sim(rate=%.1f/s dt=%.2fms tot=%llu) "
        "alt(rate=%.1f/s dt=%.4fms tot=%llu) "
        "particle_dt(rate=%.1f/s tot=%llu) "
        "flt=%.6f dword_ms=%u "
        "| wm=%s create=%llu reset=%llu cds=%llu",
        sec_since,
        g_enabled.load(std::memory_order_relaxed) ? "ON" : "OFF",
        ts_name, sim_hz,
        throttling ? 1 : 0, minigame ? 1 : 0,
        sim_this_frm ? 1 : 0,
        top[0] ? top : "(none)",
        static_cast<double>(r_dlt) / sec_since,
        g_last_render_dt.load(std::memory_order_relaxed) * 1000.0,
        static_cast<unsigned long long>(r_now),
        static_cast<double>(s_dlt) / sec_since,
        g_last_sim_dt.load(std::memory_order_relaxed) * 1000.0,
        static_cast<unsigned long long>(s_now),
        static_cast<double>(a_dlt) / sec_since,
        g_last_alt_pump_dt.load(std::memory_order_relaxed) * 1000.0,
        static_cast<unsigned long long>(a_now),
        static_cast<double>(p_dlt) / sec_since,
        static_cast<unsigned long long>(p_now),
        flt_now, ms_now,
        wm_on ? "ON" : "OFF", wm_create, wm_reset, wm_cds);
}

void write_for_sim_run(double elapsed_seconds) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    // Apply time-scale to sim-path consumers (entity_transform_tick,
    // physics_state_machine_tick, anim_update_all_tracks, etc.).
    // RealTime → unchanged. SlowMoAtLowSim → multiplied by sim_hz/60
    // so game logic ALSO slows in slow-mo mode (otherwise sim continues
    // at real-time while only render slows, producing artifact).
    const float scale = compute_render_dt_scale();
    const float dt = clamp_dt(static_cast<float>(elapsed_seconds * scale));
    write_flt_const(dt);
    g_last_sim_dt.store(dt, std::memory_order_relaxed);
    g_sim_writes.fetch_add(1, std::memory_order_relaxed);
}

} // namespace mtr::dt_correctness

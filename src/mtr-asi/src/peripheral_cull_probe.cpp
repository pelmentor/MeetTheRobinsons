// Peripheral-cull diagnostic probe.
//
// Per [research/findings/peripheral-cull-pipeline-2026-05-09.md], the
// "circular pattern at screen corners" symptom is produced by
// `cull_aabb_corners_vs_global_frustum` at 0x4E0370 reading from a single
// global frustum struct `g_cull_frustum` at 0x726498. Prior overrides on
// the per-camera projection cache (`outer+0xD4`) did not affect this gate
// because the per-object cull consults a separate global.
//
// This module gives us live evidence:
//   1. Per-frame DELTA of the engine's two cull counters
//      (`g_cull_count_sphere_reject` at 0x724EC4 + `g_cull_count_corner_reject`
//      at 0x724EC8) — tells us which stage rejects how many objects per frame.
//   2. Live snapshot of the 7 planes inside `g_cull_frustum[+128..+236]` —
//      tells us what frustum the engine is actually testing against, and
//      whether the side planes match the expected 16:9 / aspect-corrected
//      values or are still 4:3.
//   3. Optional force-pass at the gate: hook `cull_aabb_corners_vs_global_frustum`
//      and short-circuit return 1. If corners stop disappearing → confirmed
//      the gate. (Disabled by default; runs the full GPU draw load when on.)
//   4. Optional force-pass at the sphere-cull stage:
//      `cull_sphere_vs_global_frustum` (0x4DFF20). Same pattern.
//   5. Per-frame count of cull dispatches (entry to 0x4E0AD0). Sanity check
//      that the cull is actually engaged.
//
// All hooks are MinHook detours on plain code (no IAT slots, no thunks);
// the cull pipeline functions are normal `__cdecl` / `__thiscall` functions
// that have been fully decompiled and verified.

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::peripheral_cull_probe {

namespace {

// =============================================================================
// Engine addresses (verified static; see findings doc)
// =============================================================================

constexpr uintptr_t kCullFrustumVA          = 0x00726498;
constexpr uintptr_t kCullFrustumPlanesOff   = 128;            // +128..+236 = 7 vec4
constexpr size_t    kCullFrustumNumPlanes   = 7;
constexpr size_t    kCullFrustumPlaneStride = 16;             // bytes
constexpr size_t    kCullFrustumPlanesBytes = kCullFrustumNumPlanes * kCullFrustumPlaneStride; // 112

constexpr uintptr_t kCullCountSphereVA = 0x00724EC4;
constexpr uintptr_t kCullCountCornerVA = 0x00724EC8;

constexpr uintptr_t kCullCornersFnVA       = 0x004E0370;  // cull_aabb_corners_vs_global_frustum
constexpr uintptr_t kCullSphereFnVA        = 0x004DFF20;  // cull_sphere_vs_global_frustum
constexpr uintptr_t kCullDispatchSphereVA  = 0x004E0AD0;  // cull_dispatch_with_sphere_and_corners

// =============================================================================
// State
// =============================================================================

std::atomic<bool> g_installed{false};

// User toggles. Both default ON (2026-05-09 user request) — pairs with
// the 50k draw_dist default. The engine's per-object cull (corner-AABB
// + sphere-vs-frustum) would otherwise still drop distant geometry even
// though the projection-cache far plane was pushed out, so distant
// detail wouldn't actually be visible without these bypasses.
std::atomic<bool> g_force_pass_corners{true};
std::atomic<bool> g_force_pass_sphere {true};

// Per-plane force-pass: when bit i is set, plane i in g_cull_frustum is
// overwritten with (0,0,0,-1) before each cull dispatch. The corner test
// at 0x4E0370 culls iff `dot(plane.n, corner) + plane.d > 0` for ALL 8
// corners — setting plane = (0,0,0,-1) makes the dot product = -1 for
// every corner, so the loop breaks on the first corner and the cull is
// skipped. Same for the sphere test (which uses the same plane data).
// We restore the original plane data after the call so subsequent passes
// in the same frame see the engine's intended values.
std::atomic<uint32_t> g_force_pass_planes{0};

// Hook trampolines (set after MH_CreateHook + MH_EnableHook).
using PFN_CullCorners = char (__cdecl*)(int a1, void* a2, void* a3);
using PFN_CullSphere  = int  (__cdecl*)(int a1, float* a2, void* a3);
using PFN_CullDispatch = char (__cdecl*)(int a1, void* a2, void* a3, int a4, char a5);

PFN_CullCorners  g_orig_corners  = nullptr;
PFN_CullSphere   g_orig_sphere   = nullptr;
PFN_CullDispatch g_orig_dispatch = nullptr;

// Per-frame counters (running). Snapshotted by frame_tick().
std::atomic<uint64_t> g_frame_dispatches      {0};
std::atomic<uint64_t> g_frame_corner_calls    {0};   // entries to corners cull (we hook detour-side)
std::atomic<uint64_t> g_frame_sphere_calls    {0};   // entries to sphere cull
std::atomic<uint64_t> g_frame_corner_forced   {0};
std::atomic<uint64_t> g_frame_sphere_forced   {0};

// "Last frame" snapshots for UI.
std::atomic<uint64_t> g_last_dispatches    {0};
std::atomic<uint64_t> g_last_corner_calls  {0};
std::atomic<uint64_t> g_last_sphere_calls  {0};
std::atomic<uint64_t> g_last_corner_forced {0};
std::atomic<uint64_t> g_last_sphere_forced {0};

// Engine cull-counter deltas (read from process memory each frame).
std::atomic<uint64_t> g_last_engine_corner_delta {0};
std::atomic<uint64_t> g_last_engine_sphere_delta {0};
uint32_t              g_prev_engine_corner_total {0};
uint32_t              g_prev_engine_sphere_total {0};

// Live plane snapshot. 7 planes × 4 floats. Updated atomically via mutex-
// free pattern: writer fills shadow buffer + bumps version; reader copies +
// re-checks version (seqlock-lite). Single writer (frame_tick), many readers
// (UI). For our purposes a plain memcpy under a relaxed atomic version is
// sufficient — UI is not frame-critical.
struct PlaneSnapshot {
    float planes[kCullFrustumNumPlanes][4];
    uint32_t version;
};
PlaneSnapshot g_planes_buf[2]{};
std::atomic<uint32_t> g_planes_version{0};

// =============================================================================
// MinHook detours
// =============================================================================

char __cdecl hk_cull_corners(int a1, void* a2, void* a3) {
    g_frame_corner_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_force_pass_corners.load(std::memory_order_relaxed)) {
        g_frame_corner_forced.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return g_orig_corners ? g_orig_corners(a1, a2, a3) : 1;
}

int __cdecl hk_cull_sphere(int a1, float* a2, void* a3) {
    g_frame_sphere_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_force_pass_sphere.load(std::memory_order_relaxed)) {
        g_frame_sphere_forced.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return g_orig_sphere ? g_orig_sphere(a1, a2, a3) : 1;
}

char __cdecl hk_cull_dispatch(int a1, void* a2, void* a3, int a4, char a5) {
    g_frame_dispatches.fetch_add(1, std::memory_order_relaxed);
    const uint32_t mask = g_force_pass_planes.load(std::memory_order_relaxed);
    if (mask == 0 || !g_orig_dispatch) {
        return g_orig_dispatch ? g_orig_dispatch(a1, a2, a3, a4, a5) : 1;
    }
    // Save + override + restore the requested planes.
    auto* planes_base = reinterpret_cast<float*>(
        kCullFrustumVA + kCullFrustumPlanesOff);
    float saved[kCullFrustumNumPlanes][4]{};
    bool  saved_valid[kCullFrustumNumPlanes]{};
    constexpr float kForcePass[4] = { 0.0f, 0.0f, 0.0f, -1.0f };
    __try {
        for (size_t i = 0; i < kCullFrustumNumPlanes; ++i) {
            if ((mask >> i) & 1u) {
                std::memcpy(saved[i], planes_base + i * 4, sizeof(saved[i]));
                std::memcpy(planes_base + i * 4, kForcePass, sizeof(kForcePass));
                saved_valid[i] = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // g_cull_frustum not yet populated — skip override; let orig run.
        return g_orig_dispatch(a1, a2, a3, a4, a5);
    }
    char result = g_orig_dispatch(a1, a2, a3, a4, a5);
    __try {
        for (size_t i = 0; i < kCullFrustumNumPlanes; ++i) {
            if (saved_valid[i]) {
                std::memcpy(planes_base + i * 4, saved[i], sizeof(saved[i]));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // unmapped during call? unlikely. Leave overridden plane in place;
        // engine will rewrite on the next setup pass.
    }
    return result;
}

// =============================================================================
// Install
// =============================================================================

bool create_and_enable(void* target, void* detour, void** trampoline_out) {
    if (MH_CreateHook(target, detour, trampoline_out) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) return false;
    return true;
}

} // namespace

bool install() {
    if (g_installed.exchange(true)) return true;

    // MinHook is initialized by mtr::dllmain on attach.

    bool ok_corners  = create_and_enable(reinterpret_cast<void*>(kCullCornersFnVA),
                                         reinterpret_cast<void*>(&hk_cull_corners),
                                         reinterpret_cast<void**>(&g_orig_corners));
    bool ok_sphere   = create_and_enable(reinterpret_cast<void*>(kCullSphereFnVA),
                                         reinterpret_cast<void*>(&hk_cull_sphere),
                                         reinterpret_cast<void**>(&g_orig_sphere));
    bool ok_dispatch = create_and_enable(reinterpret_cast<void*>(kCullDispatchSphereVA),
                                         reinterpret_cast<void*>(&hk_cull_dispatch),
                                         reinterpret_cast<void**>(&g_orig_dispatch));

    mtr::log::info("peripheral_cull_probe: corners=%d sphere=%d dispatch=%d (orig=%p/%p/%p)",
                   ok_corners ? 1 : 0, ok_sphere ? 1 : 0, ok_dispatch ? 1 : 0,
                   g_orig_corners, g_orig_sphere, g_orig_dispatch);
    return ok_corners && ok_sphere && ok_dispatch;
}

// =============================================================================
// Frame tick
// =============================================================================

void frame_tick() {
    // Snapshot per-frame counters (running -> last).
    g_last_dispatches   .store(g_frame_dispatches   .exchange(0));
    g_last_corner_calls .store(g_frame_corner_calls .exchange(0));
    g_last_sphere_calls .store(g_frame_sphere_calls .exchange(0));
    g_last_corner_forced.store(g_frame_corner_forced.exchange(0));
    g_last_sphere_forced.store(g_frame_sphere_forced.exchange(0));

    // Engine cull counters: read raw uint32_t totals, compute delta from
    // previous frame. The engine globals are incremented unboundedly; we
    // care about the rate per frame.
    __try {
        const uint32_t corner_total = *reinterpret_cast<volatile uint32_t*>(kCullCountCornerVA);
        const uint32_t sphere_total = *reinterpret_cast<volatile uint32_t*>(kCullCountSphereVA);
        const uint32_t corner_delta = corner_total - g_prev_engine_corner_total;
        const uint32_t sphere_delta = sphere_total - g_prev_engine_sphere_total;
        g_prev_engine_corner_total = corner_total;
        g_prev_engine_sphere_total = sphere_total;
        g_last_engine_corner_delta.store(corner_delta);
        g_last_engine_sphere_delta.store(sphere_delta);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Globals not mapped? Should not happen on this build.
    }

    // Live plane snapshot. Read the 7 planes from g_cull_frustum[+128..+236].
    __try {
        const uint32_t v = g_planes_version.load(std::memory_order_relaxed);
        const uint32_t next = v + 1;
        const uint32_t slot = next & 1;
        std::memcpy(g_planes_buf[slot].planes,
                    reinterpret_cast<const void*>(kCullFrustumVA + kCullFrustumPlanesOff),
                    kCullFrustumPlanesBytes);
        g_planes_buf[slot].version = next;
        g_planes_version.store(next, std::memory_order_release);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Frustum not yet populated (very early frames); skip.
    }
}

// =============================================================================
// Public reader API
// =============================================================================

bool installed() { return g_installed.load(); }

bool force_pass_corners() { return g_force_pass_corners.load(); }
bool force_pass_sphere () { return g_force_pass_sphere .load(); }

void set_force_pass_corners(bool v) {
    g_force_pass_corners.store(v);
    mtr::log::info("peripheral_cull_probe: force_pass_corners = %d", v ? 1 : 0);
}
void set_force_pass_sphere(bool v) {
    g_force_pass_sphere.store(v);
    mtr::log::info("peripheral_cull_probe: force_pass_sphere = %d", v ? 1 : 0);
}

bool force_pass_plane(int idx) {
    if (idx < 0 || idx >= static_cast<int>(kCullFrustumNumPlanes)) return false;
    return (g_force_pass_planes.load() >> idx) & 1u;
}
void set_force_pass_plane(int idx, bool v) {
    if (idx < 0 || idx >= static_cast<int>(kCullFrustumNumPlanes)) return;
    const uint32_t bit = 1u << idx;
    if (v) g_force_pass_planes.fetch_or(bit);
    else   g_force_pass_planes.fetch_and(~bit);
    mtr::log::info("peripheral_cull_probe: force_pass_plane[%d] = %d", idx, v ? 1 : 0);
}
uint32_t force_pass_plane_mask() { return g_force_pass_planes.load(); }
void set_force_pass_plane_mask(uint32_t mask) {
    g_force_pass_planes.store(mask & ((1u << kCullFrustumNumPlanes) - 1u));
    mtr::log::info("peripheral_cull_probe: force_pass_plane_mask = 0x%X", mask);
}

uint64_t last_dispatches    () { return g_last_dispatches   .load(); }
uint64_t last_corner_calls  () { return g_last_corner_calls .load(); }
uint64_t last_sphere_calls  () { return g_last_sphere_calls .load(); }
uint64_t last_corner_forced () { return g_last_corner_forced.load(); }
uint64_t last_sphere_forced () { return g_last_sphere_forced.load(); }
uint64_t last_engine_corner_delta() { return g_last_engine_corner_delta.load(); }
uint64_t last_engine_sphere_delta() { return g_last_engine_sphere_delta.load(); }

// Copy the latest 7-plane snapshot. Returns false if no snapshot yet.
bool snapshot_planes(float out[7][4]) {
    const uint32_t v = g_planes_version.load(std::memory_order_acquire);
    if (v == 0) return false;
    const uint32_t slot = v & 1;
    std::memcpy(out, g_planes_buf[slot].planes, kCullFrustumPlanesBytes);
    return true;
}

} // namespace mtr::peripheral_cull_probe

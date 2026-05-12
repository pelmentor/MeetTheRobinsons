// View interpolation client (M3.1).
//
// On each render frame, after the camera_apply orchestrator captures the
// fresh prev/curr snapshots, this module slerps the rotation top-3x3 and
// either lerps the translation row directly (legacy) or extracts +
// reconstructs the camera world position (camera-world-space, default ON
// because it's mathematically correct for view matrices specifically).
//
// Skipped when:
//   - feature toggle off (default)
//   - cut detected on this sim window
//   - throttle isn't engaged (no in-between frames to interpolate across)
//   - fewer than two valid snapshots yet
//
// World matrix at 0x724C50 gets the general slerp+lerp path because it
// isn't necessarily a view matrix (typically identity-ish for the main
// scene, so the choice has minor visible effect either way).

#include "interp_internal.h"
#include "mtr/interp.h"
#include "mtr/sim_decouple.h"

#include <atomic>
#include <cstring>
#include <cstdint>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::interp {

namespace {

std::atomic<bool>     g_view_interp_enabled{false};
std::atomic<uint64_t> g_view_interp_writes{0};

// Camera-world-space view interp (mathematically correct alternative to
// direct view-matrix lerp). Default ON because it's strictly better for
// view matrices specifically. Toggle exposed for A/B testing.
std::atomic<bool>     g_view_interp_camspace{true};

} // namespace

namespace detail {

void view_apply_interp_for_render_frame(float alpha) {
    if (!g_view_interp_enabled.load(std::memory_order_relaxed)) return;
    if (!has_two_snapshots())                                   return;
    if (is_cut_detected())                                      return;
    if (!mtr::sim_decouple::effective_is_throttling())          return;

    const Snapshot& p = prev();
    const Snapshot& c = curr();

    float out_view [16];
    float out_world[16];
    if (g_view_interp_camspace.load(std::memory_order_relaxed)) {
        compose_view_interp_camspace(p.view, c.view, alpha, out_view);
    } else {
        compose_interp_matrix(p.view, c.view, alpha, out_view);
    }
    // World matrix is NOT necessarily a view matrix — keep general
    // slerp+lerp for it.
    compose_interp_matrix(p.world, c.world, alpha, out_world);

    std::memcpy(reinterpret_cast<void*>(kViewMatrixGlobalVA),  out_view,  sizeof(out_view));
    std::memcpy(reinterpret_cast<void*>(kWorldMatrixGlobalVA), out_world, sizeof(out_world));
    g_view_interp_writes.fetch_add(1, std::memory_order_relaxed);
}

} // namespace detail

// === Public API ============================================================

bool view_interp_enabled() { return g_view_interp_enabled.load(std::memory_order_relaxed); }
void set_view_interp_enabled(bool on) {
    const bool prev_v = g_view_interp_enabled.exchange(on, std::memory_order_relaxed);
    if (prev_v != on) {
        mtr::log::info("interp: view_interp %s", on ? "enabled" : "disabled");
    }
}
uint64_t view_interp_writes() { return g_view_interp_writes.load(std::memory_order_relaxed); }

bool view_interp_camspace() { return g_view_interp_camspace.load(std::memory_order_relaxed); }
void set_view_interp_camspace(bool on) {
    const bool prev_v = g_view_interp_camspace.exchange(on, std::memory_order_relaxed);
    if (prev_v != on) {
        mtr::log::info("interp: view_interp_camspace %s", on ? "enabled" : "disabled");
    }
}

} // namespace mtr::interp

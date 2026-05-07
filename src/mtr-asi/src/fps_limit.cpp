// FPS limiter -- spin-block at the end of each frame to enforce a target
// frame interval. Called from hk_EndScene AFTER g_orig_EndScene returns, so
// the simulation tick has run and the frame is already committed to D3D.
// Present (and any vsync wait it imposes) happens AFTER our spin.
//
// Strategy: target_dt = qpc_freq / target_fps. Loop until elapsed >= target_dt:
//   >1.5ms remaining -> Sleep(1)         (give CPU back, ~1-2ms accuracy)
//   200us..1.5ms     -> SwitchToThread() (yield without blocking)
//   <200us           -> busy-spin        (sub-ms accuracy without timer noise)
//
// We deliberately don't call timeBeginPeriod(1) -- process-global side effect,
// and modern Windows already gives ~1ms scheduling to recent foreground apps.
//
// Why this is a root-cause cap, not a crutch:
//   - game_get_time_ms (0x4A3CCE) is QPC-based; 30+ engine functions use it
//     for animation/AI/physics dt-scaling. Logic is dt-driven, not tick-counted.
//   - sub_57A2C0 (per-frame time-step) clamps dt + writes seconds-deltas for
//     downstream consumers -- a higher or lower frame rate doesn't desync them.
//   - Hook surface is hk_EndScene, which we already own for ImGui drawing.

#include <windows.h>
#include <atomic>
#include <cstdint>

namespace mtr::fps_limit {

namespace {

std::atomic<int> g_target{0};   // 0 = disabled, otherwise target fps
uint64_t g_qpc_freq = 0;
uint64_t g_last_qpc = 0;

uint64_t qpc_now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart);
}

} // namespace

bool enabled() { return g_target.load() > 0; }
int  current() { return g_target.load(); }

void set(int fps) {
    if (fps < 0) fps = 0;
    g_target.store(fps);
}

int monitor_refresh_hz() {
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        return static_cast<int>(dm.dmDisplayFrequency);
    }
    return 60;
}

void tick() {
    const int target = g_target.load();
    if (target <= 0) {
        // Reset baseline so re-enabling doesn't compute against a stale stamp.
        g_last_qpc = 0;
        return;
    }
    if (!g_qpc_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_freq = static_cast<uint64_t>(f.QuadPart);
        if (!g_qpc_freq) return;
    }
    const uint64_t target_dt = g_qpc_freq / static_cast<uint64_t>(target);

    if (g_last_qpc == 0) {
        g_last_qpc = qpc_now();
        return;
    }

    while (true) {
        const uint64_t now = qpc_now();
        const uint64_t elapsed = now - g_last_qpc;
        if (elapsed >= target_dt) {
            g_last_qpc = now;
            break;
        }
        const uint64_t remaining_us = (target_dt - elapsed) * 1'000'000ULL / g_qpc_freq;
        if (remaining_us > 1500) {
            Sleep(1);
        } else if (remaining_us > 200) {
            SwitchToThread();
        }
        // else: busy-spin
    }
}

} // namespace mtr::fps_limit

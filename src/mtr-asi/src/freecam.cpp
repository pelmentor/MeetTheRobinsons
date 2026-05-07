// FreeCam - native, crutchless implementation.
//
// Hook point: camera_apply_all_active (sub_4C1E40), POST.
// camera_apply_all_active is called once per frame by render_frame_top_level
// (sub_4D22D0). Internally it iterates g_active_camera_array, calls each
// camera's per-camera apply (sub_4C1BA0), which writes:
//   *(outer+0x34) view-matrix-ptr -> g_d3d_view_matrix_global @ 0x724C10
//   matrix4_invert_affine(view) ->                              0x724C50  (world)
// Whichever camera is "active" (PathCam during gameplay, ScriptCam during
// scripted shots, deathcam after death, etc.) ends up writing those globals.
// Hooking PathCam_tick alone misses the others -- that's why hitting walls
// "locked" the camera (a transient ScriptCam took over and its view won).
//
// Doing it after camera_apply_all_active means: ALL camera classes' state
// stays coherent (engine ticks them, applies them, all internal logic runs),
// then we last-write to the SAME globals the engine just wrote. The render
// pipeline downstream of the apply (culling, frustum, scene render) reads
// those same globals -- our pose propagates everywhere.
//
// Coordinate system: RH (engine uses game_PerspectiveFovRH, verified). Y-up.
//   forward(world) = (-sin(yaw)*cos(pitch), sin(pitch), cos(yaw)*cos(pitch))
//   yaw=0 looks +Z; yaw+ turns toward camera-right. Pitch+ tilts up.
//
// Activation: F3 toggles. On first activation we seed pose from the engine's
// current view matrix so the camera doesn't snap to (0,0,0). Mouse-look is
// primary (cursor-recenter); arrows are fallback. Mouse wheel scales
// move_speed. MMB requests teleport (currently a discovery dump until we
// resolve PathCam's target name -> player entity).

#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::freecam {

namespace {

// Engine global view/world matrices. Written by sub_4C1BA0 (per-camera apply)
// during camera_apply_all_active. Read by everything downstream of the apply
// (culling, frustum extraction, render). We last-write here in our POST-hook.
constexpr uintptr_t kViewMatrixGlobalVA  = 0x00724C10;
constexpr uintptr_t kWorldMatrixGlobalVA = 0x00724C50;

struct State {
    bool  active            = false;
    bool  pose_seeded       = false;
    float pos[3]            = { 0.0f, 0.0f, 0.0f };
    float yaw               = 0.0f;
    float pitch             = 0.0f;
    float move_speed        = 25.0f;
    float look_speed        = 1.5f;     // arrow-keys fallback (rad/s)
    float mouse_sens        = 0.003f;   // radians per pixel
    float fast_mult         = 4.0f;
    float min_speed         = 1.0f;
    float max_speed         = 2000.0f;
};

State            g_st;
std::mutex       g_mu;
std::atomic<ULONGLONG> g_last_tick_ms{0};

// Mouse-look uses a "recenter to window center" approach: each tick we
// compute the client-area center in screen coords, take dx/dy = cursor -
// center, apply, then SetCursorPos back to center. Anchoring to a fixed
// position (set on first tick) had a fatal flaw -- in fullscreen-exclusive
// the OS can pin the cursor at screen edges, where movement in one
// direction is clipped to 0 and yaw/pitch can no longer rotate that way.
// Window-center recenter avoids the issue entirely.
bool              g_mouse_skip_first_delta = true;

// Set by menu/console when their UI is visible -- we should NOT grab the
// cursor for mouse-look in that case (let ImGui own it).
std::atomic<bool> g_ui_visible{false};

// Captured each PathCam_tick (set by d3d9_hook). Used by MMB-teleport to
// navigate from controller -> target/player object once we figure out the
// resolution path.
std::atomic<void*> g_last_controller{nullptr};

// MMB latch: edge-triggered request set by the menu's input poll; consumed
// in tick().
std::atomic<int> g_pending_mmb{0};

// Mouse-wheel accumulator. Filled by the WH_MOUSE_LL hook (in menu.cpp);
// drained by tick() to scale move_speed.
std::atomic<int> g_wheel_accum{0};

constexpr float kPitchLimit = 1.5533f;  // ~89 deg

// Compute camera basis. RH. yaw=0 looks +Z; positive yaw rotates the look
// vector toward the camera's RIGHT (-X in world). Hence the sign on sy below.
void compute_basis(float yaw, float pitch,
                   float fwd[3], float xaxis[3], float yaxis[3], float zaxis[3]) {
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);

    fwd[0] = -sy * cp;
    fwd[1] =  sp;
    fwd[2] =  cy * cp;

    // RH view: zaxis = -forward
    zaxis[0] = -fwd[0];  zaxis[1] = -fwd[1];  zaxis[2] = -fwd[2];

    // xaxis = normalise(cross((0,1,0), zaxis)) = (zaxis.z, 0, -zaxis.x)
    xaxis[0] =  zaxis[2];
    xaxis[1] =  0.0f;
    xaxis[2] = -zaxis[0];
    const float xlen = std::sqrt(xaxis[0]*xaxis[0] + xaxis[2]*xaxis[2]);
    if (xlen > 1e-6f) {
        xaxis[0] /= xlen; xaxis[2] /= xlen;
    } else {
        xaxis[0] = 1.0f; xaxis[2] = 0.0f;
    }

    // yaxis = cross(zaxis, xaxis)
    yaxis[0] = zaxis[1]*xaxis[2] - zaxis[2]*xaxis[1];
    yaxis[1] = zaxis[2]*xaxis[0] - zaxis[0]*xaxis[2];
    yaxis[2] = zaxis[0]*xaxis[1] - zaxis[1]*xaxis[0];
}

bool key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// LookAt-RH style view matrix from current pose. Caller holds g_mu.
void build_view_matrix_locked(D3DMATRIX* out) {
    float fwd[3], xax[3], yax[3], zax[3];
    compute_basis(g_st.yaw, g_st.pitch, fwd, xax, yax, zax);

    out->m[0][0] = xax[0]; out->m[0][1] = yax[0]; out->m[0][2] = zax[0]; out->m[0][3] = 0.0f;
    out->m[1][0] = xax[1]; out->m[1][1] = yax[1]; out->m[1][2] = zax[1]; out->m[1][3] = 0.0f;
    out->m[2][0] = xax[2]; out->m[2][1] = yax[2]; out->m[2][2] = zax[2]; out->m[2][3] = 0.0f;
    out->m[3][0] = -(g_st.pos[0]*xax[0] + g_st.pos[1]*xax[1] + g_st.pos[2]*xax[2]);
    out->m[3][1] = -(g_st.pos[0]*yax[0] + g_st.pos[1]*yax[1] + g_st.pos[2]*yax[2]);
    out->m[3][2] = -(g_st.pos[0]*zax[0] + g_st.pos[1]*zax[1] + g_st.pos[2]*zax[2]);
    out->m[3][3] = 1.0f;
}

// World matrix is the inverse of the view matrix. For a rigid LookAt-RH view
// (orthonormal rotation + translation), the inverse has the rotation
// transposed and translation = -R^T * t = eye position in world. Caller
// holds g_mu.
void build_world_matrix_locked(D3DMATRIX* out) {
    float fwd[3], xax[3], yax[3], zax[3];
    compute_basis(g_st.yaw, g_st.pitch, fwd, xax, yax, zax);

    // Columns of world rotation = view-axes rows = world-axes.
    out->m[0][0] = xax[0]; out->m[0][1] = xax[1]; out->m[0][2] = xax[2]; out->m[0][3] = 0.0f;
    out->m[1][0] = yax[0]; out->m[1][1] = yax[1]; out->m[1][2] = yax[2]; out->m[1][3] = 0.0f;
    out->m[2][0] = zax[0]; out->m[2][1] = zax[1]; out->m[2][2] = zax[2]; out->m[2][3] = 0.0f;
    out->m[3][0] = g_st.pos[0]; out->m[3][1] = g_st.pos[1]; out->m[3][2] = g_st.pos[2]; out->m[3][3] = 1.0f;
}

} // namespace

bool active() {
    std::scoped_lock lock(g_mu);
    return g_st.active;
}

void set_active(bool a) {
    std::scoped_lock lock(g_mu);
    if (a == g_st.active) return;
    g_st.active = a;
    if (a) {
        // Seed pose from next post-tick engine view-matrix overwrite.
        g_st.pose_seeded = false;
    }
    mtr::log::info("freecam: active=%d (yaw=%.2f pitch=%.2f pos=%.1f,%.1f,%.1f)",
                   a ? 1 : 0, g_st.yaw, g_st.pitch,
                   g_st.pos[0], g_st.pos[1], g_st.pos[2]);
}

void get_pose(float pos[3], float* yaw, float* pitch) {
    std::scoped_lock lock(g_mu);
    pos[0] = g_st.pos[0]; pos[1] = g_st.pos[1]; pos[2] = g_st.pos[2];
    if (yaw)   *yaw   = g_st.yaw;
    if (pitch) *pitch = g_st.pitch;
}

float move_speed() { std::scoped_lock lock(g_mu); return g_st.move_speed; }
float look_speed() { std::scoped_lock lock(g_mu); return g_st.look_speed; }
float mouse_sens() { std::scoped_lock lock(g_mu); return g_st.mouse_sens; }
void  set_move_speed(float v) { std::scoped_lock lock(g_mu); g_st.move_speed = v; }
void  set_look_speed(float v) { std::scoped_lock lock(g_mu); g_st.look_speed = v; }
void  set_mouse_sens(float v) { std::scoped_lock lock(g_mu); g_st.mouse_sens = v; }

void set_ui_visible(bool v)        { g_ui_visible.store(v); }
void set_last_controller(void* c)  { g_last_controller.store(c); }
void request_teleport_to_camera()  { g_pending_mmb.store(1); }

// Called by the WH_MOUSE_LL hook (menu.cpp). delta is signed wheel ticks
// (typically +-120 per click). Accumulated and consumed in tick().
void accumulate_wheel(int delta) {
    g_wheel_accum.fetch_add(delta);
}

void tick() {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG prev = g_last_tick_ms.exchange(now);
    if (prev == 0) return;
    float dt = static_cast<float>(now - prev) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;

    const bool ui_open = g_ui_visible.load();
    const int pending_mmb = g_pending_mmb.exchange(0);
    const int wheel = g_wheel_accum.exchange(0);

    std::scoped_lock lock(g_mu);
    if (!g_st.active) {
        g_mouse_skip_first_delta = true;
        return;
    }

    // Wheel adjusts move_speed exponentially. ~120 units per click; 5% per
    // unit. So one click ~= 6x boost; one click down ~= 6x reduction. Clamp
    // to [min_speed, max_speed].
    if (wheel != 0) {
        const float factor = std::exp(static_cast<float>(wheel) * 0.0015f);
        g_st.move_speed *= factor;
        if (g_st.move_speed < g_st.min_speed) g_st.move_speed = g_st.min_speed;
        if (g_st.move_speed > g_st.max_speed) g_st.move_speed = g_st.max_speed;
    }

    const bool fast = key_down(VK_SHIFT);
    const float vmove = g_st.move_speed * (fast ? g_st.fast_mult : 1.0f) * dt;
    const float vlook = g_st.look_speed * dt;

    // Mouse-look (PRIMARY). Recenter to the foreground window's client-area
    // center each frame; delta = cursor - center; SetCursorPos back to center.
    // The center recentering keeps deltas symmetric in both directions even
    // in fullscreen-exclusive (where the OS pins the cursor at edges of the
    // working area, which would otherwise clip leftward/upward delta to 0).
    if (!ui_open) {
        HWND hwnd = GetForegroundWindow();
        RECT rc;
        if (hwnd && GetClientRect(hwnd, &rc)) {
            POINT center{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
            ClientToScreen(hwnd, &center);

            POINT cur;
            if (GetCursorPos(&cur)) {
                if (!g_mouse_skip_first_delta) {
                    const int dx = cur.x - center.x;
                    const int dy = cur.y - center.y;
                    if (dx != 0 || dy != 0) {
                        g_st.yaw   += static_cast<float>(dx) * g_st.mouse_sens;
                        g_st.pitch -= static_cast<float>(dy) * g_st.mouse_sens;
                    }
                }
            }
            // Always recenter, even on the first tick after activation -- the
            // game / OS may have parked the cursor anywhere; resetting to the
            // window center every frame guarantees the next read is symmetric.
            SetCursorPos(center.x, center.y);
            g_mouse_skip_first_delta = false;
        }
    } else {
        g_mouse_skip_first_delta = true;
    }

    float fwd[3], xax[3], yax[3], zax[3];
    compute_basis(g_st.yaw, g_st.pitch, fwd, xax, yax, zax);

    if (key_down('W')) for (int i = 0; i < 3; ++i) g_st.pos[i] += fwd[i] * vmove;
    if (key_down('S')) for (int i = 0; i < 3; ++i) g_st.pos[i] -= fwd[i] * vmove;
    if (key_down('D')) for (int i = 0; i < 3; ++i) g_st.pos[i] += xax[i] * vmove;
    if (key_down('A')) for (int i = 0; i < 3; ++i) g_st.pos[i] -= xax[i] * vmove;
    if (key_down(VK_SPACE)) g_st.pos[1] += vmove;
    if (key_down('C'))      g_st.pos[1] -= vmove;

    // Arrow keys = fallback look (consistent with mouse: right = right turn).
    if (key_down(VK_LEFT))  g_st.yaw   -= vlook;
    if (key_down(VK_RIGHT)) g_st.yaw   += vlook;
    if (key_down(VK_UP))    g_st.pitch += vlook;
    if (key_down(VK_DOWN))  g_st.pitch -= vlook;

    if (g_st.pitch >  kPitchLimit) g_st.pitch =  kPitchLimit;
    if (g_st.pitch < -kPitchLimit) g_st.pitch = -kPitchLimit;

    // MMB pressed: discovery dump until we resolve target name -> player.
    if (pending_mmb) {
        void* ctrl = g_last_controller.load();
        mtr::log::info("freecam: MMB pressed -- camera pos=(%.2f, %.2f, %.2f) yaw=%.3f pitch=%.3f",
                       g_st.pos[0], g_st.pos[1], g_st.pos[2], g_st.yaw, g_st.pitch);
        if (!ctrl) {
            mtr::log::info("freecam: MMB -- no controller captured yet");
        } else {
            const uint8_t* base = static_cast<const uint8_t*>(ctrl);
            const uint32_t target = *reinterpret_cast<const uint32_t*>(base + 0x34);
            mtr::log::info("freecam: ctrl=%p  *(ctrl+0x34)=0x%08X  *(ctrl+0x38)=0x%08X  *(ctrl+0x3C)=0x%08X",
                           ctrl,
                           target,
                           *reinterpret_cast<const uint32_t*>(base + 0x38),
                           *reinterpret_cast<const uint32_t*>(base + 0x3C));
            if (target) {
                const uint32_t* h = reinterpret_cast<const uint32_t*>(target);
                for (int i = 0; i < 32; ++i) {
                    const uint32_t u = h[i];
                    float f; std::memcpy(&f, &u, 4);
                    const bool is_finite = (f == f) && (f > -1e30f) && (f < 1e30f);
                    if (is_finite && f != 0.0f && (f > -1e6f && f < 1e6f) &&
                        !(f > -1e-30f && f < 1e-30f)) {
                        mtr::log::info("freecam: target+%-3d (0x%02X) = %12.4f  (0x%08X)",
                                       i*4, i*4, f, u);
                    } else {
                        mtr::log::info("freecam: target+%-3d (0x%02X) = 0x%08X",
                                       i*4, i*4, u);
                    }
                }
            }
        }
    }
}

// Public: build a LookAt-RH view matrix from the current freecam pose.
// Used by d3d9_hook's per-camera apply hook to overwrite the engine's view
// before sub_4C1BA0 propagates it to globals + D3D.
bool build_view_matrix(D3DMATRIX* out) {
    if (!out) return false;
    std::scoped_lock lock(g_mu);
    if (!g_st.active) return false;
    build_view_matrix_locked(out);
    return true;
}

void on_engine_view(const D3DMATRIX& vm) {
    std::scoped_lock lock(g_mu);
    if (!g_st.active || g_st.pose_seeded) return;

    // RH-LookAt: cols of the rotation 3x3 are view-space axes in world coords.
    const float xax[3] = { vm.m[0][0], vm.m[1][0], vm.m[2][0] };
    const float yax[3] = { vm.m[0][1], vm.m[1][1], vm.m[2][1] };
    const float zax[3] = { vm.m[0][2], vm.m[1][2], vm.m[2][2] };

    // eye = -t.x*xaxis - t.y*yaxis - t.z*zaxis.
    const float tx = vm.m[3][0], ty = vm.m[3][1], tz = vm.m[3][2];
    g_st.pos[0] = -(tx*xax[0] + ty*yax[0] + tz*zax[0]);
    g_st.pos[1] = -(tx*xax[1] + ty*yax[1] + tz*zax[1]);
    g_st.pos[2] = -(tx*xax[2] + ty*yax[2] + tz*zax[2]);

    // forward = -zaxis. With our convention forward.x = -sin(yaw)*cos(pitch),
    // so yaw = atan2(-forward.x, forward.z). pitch = asin(forward.y).
    const float fwd[3] = { -zax[0], -zax[1], -zax[2] };
    g_st.pitch = std::asin(fwd[1]);
    g_st.yaw   = std::atan2(-fwd[0], fwd[2]);
    g_st.pose_seeded = true;

    mtr::log::info("freecam: seeded from engine view -- pos=(%.1f,%.1f,%.1f) yaw=%.3f pitch=%.3f",
                   g_st.pos[0], g_st.pos[1], g_st.pos[2], g_st.yaw, g_st.pitch);
}

// Called from d3d9_hook AFTER camera_apply_all_active runs. Engine's per-camera
// applies have already written the active camera's view/world to the globals
// (0x724C10 / 0x724C50). We last-write our pose so culling/render see it.
void apply_to_globals() {
    if (!active()) return;

    D3DMATRIX* view_global  = reinterpret_cast<D3DMATRIX*>(kViewMatrixGlobalVA);
    D3DMATRIX* world_global = reinterpret_cast<D3DMATRIX*>(kWorldMatrixGlobalVA);

    // Seed from engine's current view (the one camera_apply just wrote)
    // before we overwrite, so first activation lands on the engine's pose.
    on_engine_view(*view_global);

    {
        std::scoped_lock lock(g_mu);
        if (!g_st.active) return;
        D3DMATRIX vm{}, wm{};
        build_view_matrix_locked(&vm);
        build_world_matrix_locked(&wm);
        std::memcpy(view_global,  &vm, sizeof(D3DMATRIX));
        std::memcpy(world_global, &wm, sizeof(D3DMATRIX));
    }
}

} // namespace mtr::freecam

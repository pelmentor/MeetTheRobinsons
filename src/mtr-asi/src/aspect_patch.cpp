// Aspect ratio target storage + monitor auto-detect.
//
// This module owns the user-chosen aspect ratio. It does NOT touch the game's
// 4/3 constant at 0x6C750C — that lives in dead code (game_GetCameraAspect's
// fallback path that is never executed). The actual rendering aspect override
// is applied by hk_BuildProjMatrix in d3d9_hook.cpp, which intercepts the
// game's projection-matrix builder (sub_562B20) and substitutes its `aspect`
// parameter with the value owned here.
//
// HUD/menu aspect override is handled separately by `mtr::sprite_matrix`
// (sprite-batcher path) + `mtr::ui_aspect_rules` (per-screen rules). The
// `mtr::aspect_ui` namespace that previously lived here was removed
// 2026-05-06 — its `BuildOrtho` / narrow-FOV consumer paths never fired
// in retail (verified via runtime logs), so it was dead storage.

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace mtr::log { void info(const char* fmt, ...); }

// Forward decls for sprite_matrix::current_factors. Owned by other modules,
// declared here so the factor resolver can compose them without #include
// soup. Mirrors the same chain hk_MatrixSetXform{A,B} + the menu Hilite
// overlay walk.
namespace mtr::screen_push     { bool current_top_name(char* out, size_t out_size); }
namespace mtr::ui_aspect_rules { float resolve_aspect(const char* screen_name); }
namespace mtr::aspect          { float current(); }

namespace mtr::aspect {

namespace {

constexpr float kGameDefault = 4.0f / 3.0f;

std::atomic<float> g_target {kGameDefault};
std::atomic<bool>  g_initialized{false};

float read_monitor_aspect() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    int w = 0, h = 0;
    if (GetMonitorInfoA(mon, &mi)) {
        w = mi.rcMonitor.right - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    } else {
        w = GetSystemMetrics(SM_CXSCREEN);
        h = GetSystemMetrics(SM_CYSCREEN);
    }
    return (h > 0) ? (static_cast<float>(w) / h) : (16.0f / 9.0f);
}

} // namespace

void init() {
    if (g_initialized.exchange(true)) return;
    const float a = read_monitor_aspect();
    g_target.store(a);
    mtr::log::info("aspect: target initialised to monitor aspect %.6f", a);
}

bool  available() { return true; }
float current()   { return g_target.load(); }
float original()  { return kGameDefault; }

bool set(float v) {
    if (!(v > 0.1f && v < 10.0f)) return false;
    g_target.store(v);
    mtr::log::info("aspect: target set to %.6f", v);
    return true;
}

} // namespace mtr::aspect

// FOV override -- engine's defproj.fov cvar is read once at camera-init and
// then cached in per-camera state, so writing the cvar via console doesn't
// propagate live. Mirror the aspect approach: own the value here, override
// at hk_BuildProjMatrix in d3d9_hook.cpp. 0 = no override (engine's value used).
namespace mtr::fov {

namespace {
std::atomic<float> g_target {0.0f};
}

float current()      { return g_target.load(); }
bool  has_override() { return g_target.load() > 0.0f; }

bool set(float v) {
    if (v < 0.0f) return false;
    if (v > 0.0f && (v < 10.0f || v > 170.0f)) return false;
    g_target.store(v);
    if (v > 0.0f) {
        mtr::log::info("fov: override set to %.2f deg", v);
    } else {
        mtr::log::info("fov: override cleared (engine value)");
    }
    return true;
}

} // namespace mtr::fov

// "Force every object visible" patch — localised 5-byte call-site rewrites
// at EVERY call site of sub_4E0B90 (the per-object visibility test, a
// stolen-byte IAT thunk that returns a struct pointer; callers use only
// AL as "is visible" flag). Replacing each `call sub_4E0B90` with
// `mov al, 1; nop*3` forces all 4 callers' visibility checks to pass.
//
// Earlier version patched only 0x4C385D (sub_4C3790's call). Insufficient:
// sub_4BC340 calls vis_test PER OBJECT and stores the result in a byte
// array (*((BYTE*)v2 + idx + 152)) — that byte array gates downstream
// rendering. Patching only the downstream site is dead because the upstream
// list-update site already filtered the object set.
//
// Patching the IAT slot at 0xF92F34 (the thunk's target) would in principle
// hit all callers in one go, but the post-unpack value of that slot is set
// at runtime and the thunk's `jmp [F92F34]` ABI is opaque (variadic args,
// uncertain calling convention). Multi-call-site patch is the robust path.
//
// All 4 sites use caller-cleanup: each is followed by `add esp, 18h` (=6
// args) — confirmed by IDA disasm. Our `mov al, 1; nop*3` leaves args on
// the stack, the `add esp, 18h` after still runs and cleans up. Safe.
//
// Caveats:
//  - Significant perf hit when on (renders the full object set + walks
//    full scene tree).
//  - Earlier observation reported transparency glitches at this site under
//    specific conditions; if you see render artifacts, toggle off.
namespace mtr::force_vis {

namespace {

struct Site { uintptr_t va; const char* tag; };

constexpr Site kSites[] = {
    {0x004BC406, "sub_4BC340 (scene-tree list update)"},   // upstream cull
    {0x004C385D, "sub_4C3790 (main render loop)"},         // downstream
    {0x004CBAC7, "sub_???? @ 4CBAC7 (orphan, real call)"}, // unanalyzed function
    {0x004E6A5A, "sub_4E6A20 (reflection probe)"},
};

uint8_t g_orig_bytes[std::size(kSites)][5] = {};
bool    g_orig_saved[std::size(kSites)]    = {};
bool    g_patched_all = false;

bool is_call_e8(const void* p) {
    return *static_cast<const uint8_t*>(p) == 0xE8;  // CALL rel32
}

bool write_bytes_at(uintptr_t va, const uint8_t* src) {
    void* p = reinterpret_cast<void*>(va);
    DWORD old_prot = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old_prot)) return false;
    std::memcpy(p, src, 5);
    DWORD tmp = 0;
    VirtualProtect(p, 5, old_prot, &tmp);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

} // namespace

bool active() { return g_patched_all; }

void set(bool on) {
    if (on == g_patched_all) return;

    if (on) {
        // mov al, 1 ; nop ; nop ; nop  -> 5 bytes
        const uint8_t patched[5] = { 0xB0, 0x01, 0x90, 0x90, 0x90 };
        int ok_count = 0;
        for (size_t i = 0; i < std::size(kSites); ++i) {
            const auto& site = kSites[i];
            void* p = reinterpret_cast<void*>(site.va);
            // Save original ONCE per site, and only if it's a CALL — guards
            // against patching a slot that's already been replaced by us
            // (so toggle off can still restore real bytes).
            if (!g_orig_saved[i]) {
                if (!is_call_e8(p)) {
                    mtr::log::info("force_vis: %s @ 0x%p is not a CALL (got 0x%02X) — SKIPPING",
                                   site.tag, p,
                                   static_cast<unsigned>(*static_cast<uint8_t*>(p)));
                    continue;
                }
                std::memcpy(g_orig_bytes[i], p, 5);
                g_orig_saved[i] = true;
            }
            if (write_bytes_at(site.va, patched)) {
                ok_count++;
                mtr::log::info("force_vis: ON  %s @ 0x%p -> mov al,1+nops",
                               site.tag, p);
            } else {
                mtr::log::info("force_vis: VirtualProtect failed at %s @ 0x%p",
                               site.tag, p);
            }
        }
        g_patched_all = (ok_count > 0);
    } else {
        for (size_t i = 0; i < std::size(kSites); ++i) {
            if (!g_orig_saved[i]) continue;
            if (write_bytes_at(kSites[i].va, g_orig_bytes[i])) {
                mtr::log::info("force_vis: OFF %s @ 0x%p restored",
                               kSites[i].tag, reinterpret_cast<void*>(kSites[i].va));
            }
        }
        g_patched_all = false;
    }
}

} // namespace mtr::force_vis

// Draw distance override -- the actual cull frustum's far plane.
// game_camera_build_view_frustum (0x4DF2C0) reads `a3` (far) from per-camera
// state at *(camera+4) and bakes it into the frustum's far plane (offsets
// +16..+31 of the frustum buffer). That frustum is what culling tests
// objects against, so the user-visible "draw distance" is determined here
// — NOT by the projection-matrix far arg (which only affects depth
// precision; overriding it just causes z-fighting).
namespace mtr::draw_dist {

namespace {
// 50k default override (2026-05-09 user request). Engine default of 1500
// units pops geometry visibly close to the camera; 50k matches the
// commonly-used preset and is comfortable for the typical play screens
// without z-fighting. User can still change/disable via the World tab.
std::atomic<float> g_target {50000.0f};
}

float current()      { return g_target.load(); }
bool  has_override() { return g_target.load() > 0.0f; }

bool set(float v) {
    if (v < 0.0f) return false;
    // No upper clamp. Above ~1e7 z-precision degrades and z-fighting kicks
    // in but that's the user's call — let them shoot themselves in the
    // foot if they want infinite-distance gameplay screenshots.
    if (v > 0.0f && v < 1.0f) return false;
    g_target.store(v);
    if (v > 0.0f) mtr::log::info("draw_dist: override = %.1f", v);
    else          mtr::log::info("draw_dist: override cleared");
    return true;
}

} // namespace mtr::draw_dist

// Scene knobs.
//
// Engine's "scene" cvar namespace is registered in sub_655FA0; each cvar is
// bound to a fixed VA. The cvars sit in a struct beginning at 0x745158
// (g_input_state_base in IDA), with the cvar entries starting at offset
// 0xE8 from the base (= 0x745240). My earlier offsets were wrong by 0x100
// because I read g_input_state_base as the cvar block start, not the larger
// owning struct -- explaining why the scene-cvar approach had no effect.
//
// Toggles here:
// - fog_disabled : write 0 to the fogEnabled cvar BYTE every frame so the
//   engine doesn't enable fog the next time it reads it. We also have a
//   SetRenderState hook for D3DRS_FOGENABLE as a belt-and-braces fallback.
// - side_cull_disabled : flag read by hk_PerCameraApply, which overwrites
//   the per-camera frustum's side planes (top/bottom/left/right) with
//   always-pass equations so the engine's view-space side culling never
//   rejects geometry. Useful when freecam is panning outside the player
//   camera's original frustum.
namespace mtr::scene {

namespace {
// Corrected VAs (cvar block base = 0x745240, derived from defaultShutdownDistance
// being at *((float*)&g_input_state_base + 68) = 0x745158 + 272 = 0x745268,
// matching its registration immediate 7623272 = 0x745268).
constexpr uintptr_t kClipNearVA     = 0x0074525C;
constexpr uintptr_t kClipFarVA      = 0x00745260;
constexpr uintptr_t kFadeSpanVA     = 0x00745264;
constexpr uintptr_t kShutdownDistVA = 0x00745268;
constexpr uintptr_t kFogEnabledVA   = 0x00745279;  // BYTE
constexpr uintptr_t kFogNearVA      = 0x0074527C;
constexpr uintptr_t kFogFarVA       = 0x00745280;
constexpr uintptr_t kFogDensityVA   = 0x00745284;

float* as_float(uintptr_t va) { return reinterpret_cast<float*>(va); }

std::atomic<bool> g_fog_disabled{false};
std::atomic<bool> g_side_cull_disabled{false};
std::atomic<bool> g_no_backface_cull{false};
}

float clip_near()           { return *as_float(kClipNearVA); }
float clip_far()            { return *as_float(kClipFarVA); }
float fade_span()           { return *as_float(kFadeSpanVA); }
float shutdown_distance()   { return *as_float(kShutdownDistVA); }
float fog_near()            { return *as_float(kFogNearVA); }
float fog_far()             { return *as_float(kFogFarVA); }
float fog_density()         { return *as_float(kFogDensityVA); }

void set_clip_near(float v)         { if (v > 0.0f) *as_float(kClipNearVA)     = v; }
void set_clip_far(float v)          { if (v > 0.0f) *as_float(kClipFarVA)      = v; }
void set_fade_span(float v)         { if (v >= 0.0f)*as_float(kFadeSpanVA)     = v; }
void set_shutdown_distance(float v) { if (v > 0.0f) *as_float(kShutdownDistVA) = v; }
void set_fog_near(float v)          { if (v >= 0.0f)*as_float(kFogNearVA)      = v; }
void set_fog_far(float v)           { if (v > 0.0f) *as_float(kFogFarVA)       = v; }

bool fog_disabled()                 { return g_fog_disabled.load(); }
bool side_cull_disabled()           { return g_side_cull_disabled.load(); }
bool no_backface_cull()             { return g_no_backface_cull.load(); }

void set_fog_disabled(bool v) {
    g_fog_disabled.store(v);
    // Mirror to the engine's cvar BYTE so the next time the engine
    // reads fogEnabled (e.g. on scene load) it sees our value.
    *reinterpret_cast<uint8_t*>(kFogEnabledVA) = v ? 0 : 1;
    mtr::log::info("scene: fog_disabled = %d", v ? 1 : 0);
}

void set_side_cull_disabled(bool v) {
    g_side_cull_disabled.store(v);
    mtr::log::info("scene: side_cull_disabled = %d", v ? 1 : 0);
}

void set_no_backface_cull(bool v) {
    g_no_backface_cull.store(v);
    mtr::log::info("scene: no_backface_cull = %d", v ? 1 : 0);
}

// Pumped each frame by hk_PerCameraApply when fog is set to disabled, so
// the BYTE doesn't drift even if the engine writes back to it.
void enforce_fog_disabled() {
    if (g_fog_disabled.load()) {
        *reinterpret_cast<uint8_t*>(kFogEnabledVA) = 0;
    }
}

} // namespace mtr::scene

// LOD + Periphery culling.
//
// Discovered via cvar_dump (the typed-X registration hooks). Two engine cvar
// groups govern this:
//
//   MeshLOD (struct base 0x745B38, defaults from sub_67FED0):
//     +0x0C  FocusDist             100.0
//     +0x10  HighDist               250.0
//     +0x14  MediumDist            500.0
//     +0x18  PeripheryAcceptDist   100.0
//     +0x1C  PeripheryRejectDist  1500.0
//     +0x20  PeripheryRejectAngle  0.39 rad (~22.5 deg)
//
//   ActorLOD (struct base ~0x745B78):
//     +0x04  LODScale              (default 1.0 — global LOD scalar)
//     +0x08  ONCAMERA
//     +0x0C  NEARCAMERA
//     +0x10  MEDIUMCAMERA
//     +0x14  FARCAMERA
//     +0x18  OFFCAMERA
//
// LODScale is the global multiplier applied to all LOD distances when the
// engine decides "is this object close enough to render at high detail".
// Setting LODScale > 1 pushes detail farther; < 1 pulls it in.
//
// (Periphery cvars at 0x745B50/0x745B54/0x745B58 were RE'd and exposed in
// an earlier UI section, but turned out to have no observable effect at
// runtime — the corner-cull symptom they were meant to address is owned
// by the per-object frustum cull at g_cull_frustum (0x726498). See
// research/findings/peripheral-cull-pipeline-2026-05-09.md. Removed to
// avoid misleading UX.)
namespace mtr::lod {

namespace {
constexpr uintptr_t kFocusDistVA            = 0x00745B44;
constexpr uintptr_t kHighDistVA             = 0x00745B48;
constexpr uintptr_t kMediumDistVA           = 0x00745B4C;

constexpr uintptr_t kLODScaleVA             = 0x00745B7C;
constexpr uintptr_t kOnCameraVA             = 0x00745B80;
constexpr uintptr_t kNearCameraVA           = 0x00745B84;
constexpr uintptr_t kMediumCameraVA         = 0x00745B88;
constexpr uintptr_t kFarCameraVA            = 0x00745B8C;
constexpr uintptr_t kOffCameraVA            = 0x00745B90;

float* as_float(uintptr_t va) { return reinterpret_cast<float*>(va); }
}

float focus_dist()              { return *as_float(kFocusDistVA); }
float high_dist()               { return *as_float(kHighDistVA); }
float medium_dist()             { return *as_float(kMediumDistVA); }

void set_focus_dist(float v)            { if (v >= 0.0f) *as_float(kFocusDistVA)            = v; }
void set_high_dist(float v)             { if (v >= 0.0f) *as_float(kHighDistVA)             = v; }
void set_medium_dist(float v)           { if (v >= 0.0f) *as_float(kMediumDistVA)           = v; }

float lod_scale()       { return *as_float(kLODScaleVA); }
float on_camera()       { return *as_float(kOnCameraVA); }
float near_camera()     { return *as_float(kNearCameraVA); }
float medium_camera()   { return *as_float(kMediumCameraVA); }
float far_camera()      { return *as_float(kFarCameraVA); }
float off_camera()      { return *as_float(kOffCameraVA); }

void set_lod_scale(float v)     { if (v > 0.0f) *as_float(kLODScaleVA)    = v; }
void set_on_camera(float v)     { if (v >= 0.0f) *as_float(kOnCameraVA)    = v; }
void set_near_camera(float v)   { if (v >= 0.0f) *as_float(kNearCameraVA)  = v; }
void set_medium_camera(float v) { if (v >= 0.0f) *as_float(kMediumCameraVA)= v; }
void set_far_camera(float v)    { if (v >= 0.0f) *as_float(kFarCameraVA)   = v; }
void set_off_camera(float v)    { if (v >= 0.0f) *as_float(kOffCameraVA)   = v; }

} // namespace mtr::lod

// Sprite-batcher matrix override.
//
// `render_sprite_batcher` (sub_4E8D30) builds its own projection + view via
// `matrix_set_via_xform_a(2.0, -2.0, 1.0)` (proj-style) and
// `matrix_set_via_xform_b(-2.0, -2.0, 0.0)` (view-style), bypassing the
// `sub_562B70` ortho builder we hook for `aspect_ui`. Static RE confirmed
// these helpers' arg constants and call shape; runtime data (Test 7b) is
// what tells us whether the HUD actually goes through this path.
//
// This namespace owns the substitution toggle + per-arg multipliers. When
// `enabled` is false (default), the hook in d3d9_hook.cpp passes args
// through unmodified — zero risk to normal rendering. When `enabled` is
// true, the hook scales each arg by the user's multiplier:
//
//     hk(a1, a2, a3) -> orig(a1 * mul_a1, a2 * mul_a2, a3 * mul_a3)
//
// User can dial in pillarbox by experimentation: for a 4:3-in-16:9
// pillarbox, the most likely correct factor is `screen / target` =
// (16/9) / (4/3) = 1.333, applied to whichever arg controls horizontal
// extent (a1 of A is the leading candidate based on the call signature).
//
// Exposed in menu only when the user has confirmed via Test 7b that the
// builder hooks fire on HUD. Until that's known, leaving the toggle off
// is the safe default.
namespace mtr::sprite_matrix {

namespace {
std::atomic<bool>  g_enabled{false};
// "auto from rules" — when enabled, ignores manual sliders and computes
// the X-pillarbox factor each frame from ui_aspect_rules + current top
// screen. This is the end-game UX: user defines per-screen rules
// (e.g. PauseMenu -> 4:3, Loading -> 16:9) and the override applies
// automatically as the user navigates.
std::atomic<bool>  g_auto_from_rules{false};
// Default multipliers = 1.0 (passthrough). When user toggles on, these are
// the actual factors applied. UI starts the user at 1.0 so they can dial
// from a known-good baseline.
std::atomic<float> g_mul_a_a1{1.0f};
std::atomic<float> g_mul_a_a2{1.0f};
std::atomic<float> g_mul_a_a3{1.0f};
std::atomic<float> g_mul_b_a1{1.0f};
std::atomic<float> g_mul_b_a2{1.0f};
std::atomic<float> g_mul_b_a3{1.0f};
// Pass-override factor: used by Phase 3 split-pass (sprite_split). When
// non-zero, the matrix hooks ignore enabled/auto/manual settings and
// apply this factor to a1 of A and B. 0.0 = no pass override; user
// toggles win. Set transiently inside the wrapper for each render pass.
std::atomic<float> g_pass_override_factor{0.0f};
// Position-offset channel: live X/Y translation applied to the sprite
// transform's translate stage (matrix B, args a1 and a2). Lets the user
// nudge UI placement in clip-space units without changing the
// pillarbox scale. Applied AFTER the pillarbox factor multiply.
std::atomic<float> g_pos_offset_x{0.0f};
std::atomic<float> g_pos_offset_y{0.0f};
} // namespace

bool enabled() { return g_enabled.load(); }
void set_enabled(bool v) {
    g_enabled.store(v);
    mtr::log::info("sprite_matrix: enabled = %d", v ? 1 : 0);
}
bool auto_from_rules() { return g_auto_from_rules.load(); }
void set_auto_from_rules(bool v) {
    g_auto_from_rules.store(v);
    mtr::log::info("sprite_matrix: auto_from_rules = %d", v ? 1 : 0);
}

float mul_a_a1() { return g_mul_a_a1.load(); }
float mul_a_a2() { return g_mul_a_a2.load(); }
float mul_a_a3() { return g_mul_a_a3.load(); }
float mul_b_a1() { return g_mul_b_a1.load(); }
float mul_b_a2() { return g_mul_b_a2.load(); }
float mul_b_a3() { return g_mul_b_a3.load(); }

void set_mul_a(float a1, float a2, float a3) {
    g_mul_a_a1.store(a1);
    g_mul_a_a2.store(a2);
    g_mul_a_a3.store(a3);
}
void set_mul_b(float a1, float a2, float a3) {
    g_mul_b_a1.store(a1);
    g_mul_b_a2.store(a2);
    g_mul_b_a3.store(a3);
}

void reset() {
    g_enabled.store(false);
    set_mul_a(1.0f, 1.0f, 1.0f);
    set_mul_b(1.0f, 1.0f, 1.0f);
}

// Pass-override channel for split-pass rendering. When non-zero, the
// matrix-builder hooks apply this factor regardless of enabled / auto /
// manual settings. Set to a target factor for the menu pass, to 1.0 for
// the HUD pass (passthrough but explicit), and back to 0.0 after.
float pass_override_factor()       { return g_pass_override_factor.load(); }
void  set_pass_override_factor(float f) { g_pass_override_factor.store(f); }

// Live position offset applied to the translate matrix (xform_b's a1/a2).
// The value is added to the post-pillarbox-factor a1 / a2 in clip-space
// units. Range -2..+2 covers most of the visible area; small values
// (~0.05) nudge the UI by a few pixels.
float pos_offset_x()              { return g_pos_offset_x.load(); }
float pos_offset_y()              { return g_pos_offset_y.load(); }
void  set_pos_offset_x(float v)   { g_pos_offset_x.store(v); }
void  set_pos_offset_y(float v)   { g_pos_offset_y.store(v); }

// Canonical resolver for the engine's effective sprite-batcher transform
// this frame. Mirrors the activation order of hk_MatrixSetXformA exactly:
//
//   1. pass_override_factor != 0  → Fx = override   (Phase 3 split-pass)
//   2. auto_from_rules + matching ui_aspect_rule  → Fx = target / screen
//   3. enabled (manual master)  → Fx = mul_a_a1, Fy = |mul_a_a2|
//   4. else  → Fx = Fy = 1.0
//
// pos_offset_x/y is always added in clip-space units (independent of F).
//
// Y semantics: the engine's WORLD scale is (2, -2, 1) — Y is flipped, so
// mul_a_a2 is negative for "no flip". current_factors returns the
// magnitude (|mul_a_a2|) as Fy so consumers compute screen coords with
// a positive scale; the actual sign-flip happens inside the engine pipe.
//
// Used by both the Hilite overlay (menu.cpp) and sprite_picking's
// engine→screen conversion. ONE source of truth — keep it that way.
struct Factors { float Fx; float Fy; float dx; float dy; };

Factors current_factors() {
    Factors r{1.0f, 1.0f,
              g_pos_offset_x.load(),
              g_pos_offset_y.load()};
    const float ovr = g_pass_override_factor.load();
    if (ovr != 0.0f) {
        r.Fx = ovr;
        return r;
    }
    if (g_auto_from_rules.load()) {
        char top[64] = {0};
        mtr::screen_push::current_top_name(top, sizeof(top));
        const float target = mtr::ui_aspect_rules::resolve_aspect(top);
        const float screen = mtr::aspect::current();
        if (target > 0.0f && screen > 0.0f) {
            r.Fx = target / screen;
            return r;
        }
    }
    if (g_enabled.load()) {
        r.Fx = g_mul_a_a1.load();
        float my = g_mul_a_a2.load();
        r.Fy = my < 0.0f ? -my : my;
    }
    return r;
}

} // namespace mtr::sprite_matrix

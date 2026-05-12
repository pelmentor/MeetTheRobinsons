// Private header for the d3d9_hook module's split TUs.
//
// d3d9_hook.cpp owns the IDirect3D9::CreateDevice + IDirect3DDevice9::EndScene/Reset
// vtable hooks (the D3D9 API surface), plus the install dispatcher.
// The split files implement engine-side hooks:
//
//   d3d9_hook/camera_hooks.cpp — per_camera_apply, build_proj_matrix,
//                                 build_frustum, camera_compute,
//                                 game_camera_tick, vis_test
//   d3d9_hook/sprite_hooks.cpp — wrap_SetTransform (+_state),
//                                 build_ortho, MatrixSetXformA/B,
//                                 SetTransform passthrough
//   d3d9_hook/diag_hooks.cpp   — SetClipPlane, SetRenderState
//                                 (CLIPPLANEENABLE filter + fog disable)
//
// install() in d3d9_hook.cpp captures vtables, then calls each TU's
// install_* dispatcher to wire up the engine-side hooks.

#pragma once

#include <windows.h>
#include <d3d9.h>
#include <cstdint>

namespace mtr::d3d9hook::detail {

// === Engine VAs ===========================================================

constexpr uintptr_t kWrapSetTransformVA      = 0x005625E0;
constexpr uintptr_t kWrapSetTransformStateVA = 0x005625C0;
constexpr uintptr_t kStateGlobalVA           = 0x006FBD58;
constexpr uintptr_t kStateMatrixBufferVA     = 0x00729E30;
constexpr uintptr_t kBuildProjMatrixVA       = 0x00562B20;
constexpr uintptr_t kBuildOrthoVA            = 0x00562B70;
constexpr uintptr_t kCameraComputeVA         = 0x00564600;
constexpr uintptr_t kGameCameraTickVA        = 0x0058C910;
constexpr uintptr_t kBuildFrustumVA          = 0x004DF2C0;
constexpr uintptr_t kCameraApplyStateVA      = 0x00564650;
constexpr uintptr_t kVisTestVA               = 0x004E0B90;
constexpr uintptr_t kMatrixSetXformAVA       = 0x00562AA0;
constexpr uintptr_t kMatrixSetXformBVA       = 0x00562AE0;
constexpr uintptr_t kPerCameraApplyVA        = 0x004C1BA0;

// === PFN types ============================================================

using PFN_CreateDevice    = HRESULT (STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                          D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PFN_EndScene        = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using PFN_Reset           = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PFN_SetTransform    = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using PFN_WrapSetTransform      = void (__cdecl*)(const D3DMATRIX*);
using PFN_WrapSetTransformState = void (__cdecl*)();
using PFN_BuildProjMatrix  = int  (__cdecl*)(float fov_deg, float aspect, float near_, float far_);
using PFN_CameraCompute    = int  (__fastcall*)(int this_, int /*edx*/);
using PFN_GameCamTick      = int  (__fastcall*)(int this_, int /*edx*/);
using PFN_PerCameraApply   = int  (__fastcall*)(void* this_, int /*edx*/, int skip_inverse);
using PFN_BuildFrustum     = int  (__cdecl*)(int out_buf, float near_, float far_,
                                              char has_clip, int extra_axis,
                                              float extra_d, float fov_deg, float aspect);
using PFN_CameraApplyState = char (__fastcall*)(int this_, int /*edx*/);
using PFN_VisTest          = int  (__fastcall*)(void* this_, int /*edx*/, int a2, int a3,
                                                 int a4, int a5, int a6, int a7);
using PFN_SetClipPlane     = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const float*);
using PFN_SetRenderState   = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using PFN_BuildOrtho       = int  (__cdecl*)(float l, float r, float t, float b, float n, float f);
using PFN_MatrixSet3       = int  (__cdecl*)(int a1, int a2, int a3);

// === Sub-TU install dispatchers ===========================================
// Called from d3d9_hook.cpp's install() after vtable capture. Each one is
// idempotent-friendly: logs failure, returns void.

void install_camera_hooks();                                     // engine VAs only
void install_sprite_hooks(void* p_SetTransform);                 // SetTransform from vtable
void install_diag_hooks(void* p_SetClipPlane, void* p_SetRenderState);

} // namespace mtr::d3d9hook::detail
